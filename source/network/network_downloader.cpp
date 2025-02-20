#include "headers.hpp"
#include "network/network_downloader.hpp"
#include "network/network_io.hpp"
#include <cassert>


// --------------------------------
// NetworkStream implementation
// --------------------------------

NetworkStream::NetworkStream(std::string url, bool whole_download, NetworkSessionList *session_list) : url(url), whole_download(whole_download), session_list(session_list) {
	svcCreateMutex(&downloaded_data_lock, false);
}
bool NetworkStream::is_data_available(u64 start, u64 size) {
	if (!ready) return false;
	if (start + size > len) return false;
	u64 end = start + size - 1;
	u64 start_block = start / BLOCK_SIZE;
	u64 end_block = end / BLOCK_SIZE;
	
	bool res = true;
	svcWaitSynchronization(downloaded_data_lock, std::numeric_limits<s64>::max());
	for (u64 block = start_block; block <= end_block; block++) if (!downloaded_data.count(block)) {
		res = false;
		break;
	}
	svcReleaseMutex(downloaded_data_lock);
	return res;
}
std::vector<u8> NetworkStream::get_data(u64 start, u64 size) {
	if (!ready) return {};
	if (!size) return {};
	u64 end = start + size - 1;
	u64 start_block = start / BLOCK_SIZE;
	u64 end_block = end / BLOCK_SIZE;
	std::vector<u8> res;
	
	svcWaitSynchronization(downloaded_data_lock, std::numeric_limits<s64>::max());
	auto itr = downloaded_data.find(start_block);
	assert(itr != downloaded_data.end());
	for (u64 block = start_block; block <= end_block; block++) {
		assert(itr->first == block);
		u64 cur_l = std::max(start, block * BLOCK_SIZE) - block * BLOCK_SIZE;
		u64 cur_r = std::min(end + 1, (block + 1) * BLOCK_SIZE) - block * BLOCK_SIZE;
		res.insert(res.end(), itr->second.begin() + cur_l, itr->second.begin() + cur_r);
		itr++;
	}
	svcReleaseMutex(downloaded_data_lock);
	return res;
}
void NetworkStream::set_data(u64 block, const std::vector<u8> &data) {
	svcWaitSynchronization(downloaded_data_lock, std::numeric_limits<s64>::max());
	downloaded_data[block] = data;
	if (downloaded_data.size() > MAX_CACHE_BLOCKS) { // ensure it doesn't cache too much and run out of memory
		u64 read_head_block = read_head / BLOCK_SIZE;
		if (downloaded_data.begin()->first < read_head_block) {
			// Util_log_save("net/dl", "free " + std::to_string(downloaded_data.begin()->first));
			downloaded_data.erase(downloaded_data.begin());
		} else {
			// Util_log_save("net/dl", "free " + std::to_string(std::prev(downloaded_data.end())->first));
			downloaded_data.erase(std::prev(downloaded_data.end()));
		}
	}
	svcReleaseMutex(downloaded_data_lock);
}
double NetworkStream::get_download_percentage() {
	svcWaitSynchronization(downloaded_data_lock, std::numeric_limits<s64>::max());
	double res = (double) downloaded_data.size() * BLOCK_SIZE / len * 100;
	svcReleaseMutex(downloaded_data_lock);
	return res;
}
std::vector<double> NetworkStream::get_buffering_progress_bar(int res_len) {
	svcWaitSynchronization(downloaded_data_lock, std::numeric_limits<s64>::max());
	std::vector<double> res(res_len);
	auto itr = downloaded_data.begin();
	for (int i = 0; i < res_len; i++) {
		u64 l = (u64) len * i / res_len;
		u64 r = std::min<u64>(len, len * (i + 1) / res_len);
		while (itr != downloaded_data.end()) {
			u64 il = itr->first * BLOCK_SIZE;
			u64 ir = std::min((itr->first + 1) * BLOCK_SIZE, len);
			if (ir <= l) itr++;
			else if (il >= r) break;
			else {
				res[i] += std::min(ir, r) - std::max(il, l);
				if (ir >= r) break;
				else itr++;
			}
		}
		res[i] /= r - l;
		res[i] *= 100;
	}
	svcReleaseMutex(downloaded_data_lock);
	return res;
}


// --------------------------------
// NetworkStreamDownloader implementation
// --------------------------------

NetworkStreamDownloader::NetworkStreamDownloader() {
	svcCreateMutex(&streams_lock, false);
}
void NetworkStreamDownloader::add_stream(NetworkStream *stream) {
	svcWaitSynchronization(streams_lock, std::numeric_limits<s64>::max());
	size_t index = (size_t) -1;
	for (size_t i = 0; i < streams.size(); i++) if (!streams[i]) {
		streams[i] = stream;
		index = i;
		break;
	}
	if (index == (size_t) -1) {
		index = streams.size();
		streams.push_back(stream);
	}
	svcReleaseMutex(streams_lock);
}

static bool thread_network_session_list_inited = false;
static NetworkSessionList thread_network_session_list;
static void confirm_thread_network_session_list_inited() {
	if (!thread_network_session_list_inited) {
		thread_network_session_list_inited = true;
		thread_network_session_list.init();
	}
}


#define LOG_THREAD_STR "net/dl"
void NetworkStreamDownloader::downloader_thread() {
	while (!thread_exit_reqeusted) {
		size_t cur_stream_index = (size_t) -1; // the index of the stream on which we will perform a download in this loop
		svcWaitSynchronization(streams_lock, std::numeric_limits<s64>::max());
		// back up 'read_head's as those can be changed from another thread
		std::vector<u64> read_heads(streams.size());
		for (size_t i = 0; i < streams.size(); i++) if (streams[i]) read_heads[i] = streams[i]->read_head;
		
		
		// find the stream to download next
		double margin_percentage_min = 1000;
		for (size_t i = 0; i < streams.size(); i++) {
			if (!streams[i]) continue;
			if (streams[i]->quit_request) {
				delete streams[i];
				streams[i] = NULL;
				continue;
			}
			if (streams[i]->error) continue;
			if (streams[i]->suspend_request) continue;
			if (!streams[i]->ready) {
				cur_stream_index = i;
				break;
			}
			if (streams[i]->whole_download) continue; // its entire content should already be downloaded
			
			u64 read_head_block = read_heads[i] / BLOCK_SIZE;
			u64 first_not_downloaded_block = read_head_block;
			while (first_not_downloaded_block < streams[i]->block_num && streams[i]->downloaded_data.count(first_not_downloaded_block)) {
				first_not_downloaded_block++;
				if (first_not_downloaded_block == read_head_block + MAX_FORWARD_READ_BLOCKS) break;
			}
			if (first_not_downloaded_block == streams[i]->block_num) continue; // no need to download this stream for now
			
			if (first_not_downloaded_block == read_head_block + MAX_FORWARD_READ_BLOCKS) continue; // no need to download this stream for now
			
			double margin_percentage;
			if (first_not_downloaded_block == read_head_block) margin_percentage = 0;
			else margin_percentage = (double) (first_not_downloaded_block * BLOCK_SIZE - read_heads[i]) / streams[i]->len * 100;
			if (margin_percentage_min > margin_percentage) {
				margin_percentage_min = margin_percentage;
				cur_stream_index = i;
			}
		}
		
		if (cur_stream_index == (size_t) -1) {
			svcReleaseMutex(streams_lock);
			usleep(20000);
			continue;
		}
		NetworkStream *cur_stream = streams[cur_stream_index];
		svcReleaseMutex(streams_lock);
		
		// whole download
		if (cur_stream->whole_download) {
			confirm_thread_network_session_list_inited();
			auto result = Access_http_get(cur_stream->session_list ? *cur_stream->session_list : thread_network_session_list, cur_stream->url, {});
			cur_stream->url = result.redirected_url;
			
			if (!result.fail && result.status_code_is_success() && result.data.size()) {
				{ // acquire necessary headers
					char *end;
					auto value = result.get_header("x-head-seqnum");
					cur_stream->seq_head = strtoll(value.c_str(), &end, 10);
					if (*end || !value.size()) {
						Util_log_save("net/dl", "failed to acquire x-head-seqnum");
						cur_stream->seq_head = -1;
						cur_stream->error = true;
					}
					value = result.get_header("x-sequence-num");
					cur_stream->seq_id = strtoll(value.c_str(), &end, 10);
					if (*end || !value.size()) {
						Util_log_save("net/dl", "failed to acquire x-sequence-num");
						cur_stream->seq_id = -1;
						cur_stream->error = true;
					}
				}
				if (!cur_stream->error) {
					cur_stream->len = result.data.size();
					cur_stream->block_num = (cur_stream->len + BLOCK_SIZE - 1) / BLOCK_SIZE;
					for (size_t i = 0; i < result.data.size(); i += BLOCK_SIZE) {
						size_t left = i;
						size_t right = std::min<size_t>(i + BLOCK_SIZE, result.data.size());
						cur_stream->set_data(i / BLOCK_SIZE, std::vector<u8>(result.data.begin() + left, result.data.begin() + right));
					}
					cur_stream->ready = true;
				}
			} else {
				Util_log_save("net/dl", "failed accessing : " + result.error);
				cur_stream->error = true;
				switch (result.status_code) {
					// these codes are returned when trying to read beyond the end of the livestream
					case HTTP_STATUS_CODE_NO_CONTENT :
					case HTTP_STATUS_CODE_NOT_FOUND :
						cur_stream->livestream_eof = true;
						break;
					// this code is returned when trying to read an ended livestream without archive
					case HTTP_STATUS_CODE_FORBIDDEN :
						cur_stream->livestream_private = true;
						break;
				}
			}
			result.finalize();
		} else {
			u64 block_reading = read_heads[cur_stream_index] / BLOCK_SIZE;
			if (cur_stream->ready) {
				while (block_reading < cur_stream->block_num && cur_stream->downloaded_data.count(block_reading)) block_reading++;
				if (block_reading == cur_stream->block_num) { // something unexpected happened
					Util_log_save(LOG_THREAD_STR, "unexpected error (trying to read beyond the end of the stream)");
					cur_stream->error = true;
					continue;
				}
			}
			// Util_log_save("net/dl", "dl next : " + std::to_string(cur_stream_index) + " " + std::to_string(block_reading));
			
			u64 start = block_reading * BLOCK_SIZE;
			u64 end = cur_stream->ready ? std::min((block_reading + 1) * BLOCK_SIZE, cur_stream->len) : (block_reading + 1) * BLOCK_SIZE;
			u64 expected_len = end - start;
			
			
			auto result = Access_http_get(cur_stream->session_list ? *cur_stream->session_list : thread_network_session_list, cur_stream->url,
				{{"Range", "bytes=" + std::to_string(start) + "-" + std::to_string(end - 1)}});
			cur_stream->url = result.redirected_url;
			
			if (!result.fail) {
				if (!cur_stream->ready) {
					auto content_range_str = result.get_header("Content-Range");
					char *slash = strchr(content_range_str.c_str(), '/');
					bool ok = false;
					if (slash) {
						char *end;
						cur_stream->len = strtoll(slash + 1, &end, 10);
						if (!*end) {
							ok = true;
							cur_stream->block_num = (cur_stream->len + BLOCK_SIZE - 1) / BLOCK_SIZE;
						} else Util_log_save(LOG_THREAD_STR, "failed to parse Content-Range : " + std::string(slash + 1));
					} else Util_log_save(LOG_THREAD_STR, "no slash in Content-Range response header");
					if (!ok) cur_stream->error = true;
				}
				if (cur_stream->ready && result.data.size() != expected_len) {
					Util_log_save(LOG_THREAD_STR, "size discrepancy : " + std::to_string(expected_len) + " -> " + std::to_string(result.data.size()));
					cur_stream->error = true;
					result.finalize();
					continue;
				}
				cur_stream->set_data(block_reading, result.data);
				cur_stream->ready = true;
			} else {
				Util_log_save("net/dl", "access failed : " + result.error);
				cur_stream->error = true;
			}
			result.finalize();
		}
	}
	Util_log_save(LOG_THREAD_STR, "Exit, deiniting...");
	for (auto stream : streams) if (stream) {
		stream->quit_request = true;
	}
}
void NetworkStreamDownloader::delete_all() {
	for (auto &stream : streams) {
		delete stream;
		stream = NULL;
	}
}


// --------------------------------
// other functions implementation
// --------------------------------

void network_downloader_thread(void *downloader_) {
	NetworkStreamDownloader *downloader = (NetworkStreamDownloader *) downloader_;
	
	downloader->downloader_thread();
	
	threadExit(0);
}

