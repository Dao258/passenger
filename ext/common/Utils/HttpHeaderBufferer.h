/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_HTTP_HEADER_BUFFERER_
#define _PASSENGER_HTTP_HEADER_BUFFERER_

#include <string>
#include <algorithm>
#include <cstddef>
#include <cassert>
#include <StaticString.h>
#include <Utils/StreamBoyerMooreHorspool.h>

namespace Passenger {

using namespace std;

/**
 * Buffers an entire HTTP header, including terminating "\r\n\r\n".
 *
 * Feed data until acceptingInput() is false. The entire HTTP header
 * will become available through getData(). Non-HTTP header data is
 * not consumed and will not be included in getData().
 *
 * This class has zero-copy support. If the first feed already contains
 * an entire HTTP header getData() will point to the fed data. Otherwise
 * this class will put data in an internal buffer, and getData() will
 * point to the internal buffer.
 *
 * This class also supports size checking through setMax(). If the HTTP
 * header exceeds this size then this bufferer will enter an error state.
 * The default max size is 128 KB.
 */
class HttpHeaderBufferer {
private:
	struct StaticData {
		StreamBMH_Occ occ;
		
		StaticData() {
			sbmh_init(NULL, &occ, (const unsigned char *) "\r\n\r\n", 4);
		}
	};
	
	static StaticData staticData;
	
	string buffer;
	StaticString data;
	unsigned int max;
	enum {
		WORKING,
		DONE,
		ERROR
	} state;
	union {
		struct StreamBMH terminatorFinder;
		char padding[SBMH_SIZE(4)];
	} u;
	
public:
	HttpHeaderBufferer() {
		sbmh_init(&u.terminatorFinder,
			&staticData.occ,
			(const unsigned char *) "\r\n\r\n",
			4);
		max = 1024 * 128;
		reset();
	}
	
	void setMax(unsigned int value) {
		max = value;
	}
	
	void reset() {
		buffer.clear();
		data = StaticString("", 0);
		sbmh_reset(&u.terminatorFinder);
		state = WORKING;
	}
	
	size_t feed(const char *data, size_t size) {
		if (state == DONE || state == ERROR) {
			return 0;
		}
		
		size_t accepted, feedSize;
		
		if (buffer.empty()) {
			feedSize = std::min<size_t>(size, max);
			accepted = sbmh_feed(&u.terminatorFinder,
				&staticData.occ,
				(const unsigned char *) "\r\n\r\n",
				4,
				(const unsigned char *) data,
				feedSize);
			if (u.terminatorFinder.found) {
				state = DONE;
				this->data = StaticString(data, accepted);
			} else if (feedSize == max) {
				state = ERROR;
				this->data = StaticString(data, accepted);
			} else {
				assert(accepted == size);
				buffer.append(data, size);
				this->data = buffer;
			}
		} else {
			feedSize = std::min<size_t>(size, max - buffer.size());
			accepted = sbmh_feed(&u.terminatorFinder,
				&staticData.occ,
				(const unsigned char *) "\r\n\r\n",
				4,
				(const unsigned char *) data,
				feedSize);
			buffer.append(data, accepted);
			this->data = buffer;
			if (u.terminatorFinder.found) {
				state = DONE;
			} else if (buffer.size() == (size_t) max) {
				state = ERROR;
			}
		}
		
		return accepted;
	}
	
	bool acceptingInput() const {
		return state == WORKING;
	}
	
	bool hasError() const {
		return state == ERROR;
	}
	
	/**
	 * Get the data that has been fed so far.
	 */
	StaticString getData() const {
		return data;
	}
};

} // namespace Passenger

#endif /* _PASSENGER_HTTP_HEADER_BUFFERER_ */
