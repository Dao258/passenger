/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2014-2017 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_SERVER_KIT_HTTP_HEADER_PARSER_H_
#define _PASSENGER_SERVER_KIT_HTTP_HEADER_PARSER_H_

#include <boost/cstdint.hpp>
#include <oxt/backtrace.hpp>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <MemoryKit/mbuf.h>
#include <ServerKit/Context.h>
#include <ServerKit/HttpRequest.h>
#include <ServerKit/HttpHeaderParserState.h>
#include <DataStructures/LString.h>
#include <DataStructures/HashedStaticString.h>
#include <LoggingKit/LoggingKit.h>
#include <Utils/StrIntUtils.h>
#include <Utils/Hasher.h>

namespace Passenger {
namespace ServerKit {


extern const HashedStaticString HTTP_CONTENT_LENGTH;
extern const HashedStaticString HTTP_TRANSFER_ENCODING;
extern const HashedStaticString HTTP_X_SENDFILE;
extern const HashedStaticString HTTP_X_ACCEL_REDIRECT;

struct HttpParseRequest {};
struct HttpParseResponse {};

template<typename Message, typename MessageType = HttpParseRequest>
class HttpHeaderParser {
private:
	Context *ctx;
	HttpHeaderParserState *state;
	Message *message;
	psg_pool_t *pool;
	const MemoryKit::mbuf *currentBuffer;
	http_method	requestMethod;

	bool validateHeader(const HttpParseRequest &tag, const Header *header) {
		if (!state->secureMode) {
			if (!psg_lstr_cmp(&header->key, P_STATIC_STRING("!~"), 2)) {
				return true;
			} else {
				if (header->key.size == 2) {
					// Security password. Check whether it hasn't been
					// given before and whether it is correct.
					if (ctx->secureModePassword.empty()
					 || psg_lstr_cmp(&header->val, ctx->secureModePassword))
					{
						state->secureMode = true;
						return true;
					} else {
						state->state = HttpHeaderParserState::ERROR_SECURITY_PASSWORD_MISMATCH;
						return false;
					}
				} else {
					// Secure header encountered without having
					// encountered a security password.
					state->state = HttpHeaderParserState::ERROR_SECURE_HEADER_NOT_ALLOWED;
					return false;
				}
			}
		} else {
			if (psg_lstr_cmp(&header->key, P_STATIC_STRING("!~"), 2)) {
				if (header->key.size == 2) {
					state->secureMode = false;
				}
				return true;
			} else {
				// To prevent Internet clients from injecting secure headers,
				// we require the web server put secure headers between a begin
				// marker (the security password header) and an end marker.
				// If we find a normal header between the markers, then we
				// can assume the web server is bugged or compromised.
				state->state = HttpHeaderParserState::ERROR_NORMAL_HEADER_NOT_ALLOWED_AFTER_SECURITY_PASSWORD;
				return false;
			}
		}
	}

	bool validateHeader(const HttpParseResponse &tag, const Header *header) {
		state->secureMode = psg_lstr_cmp(&header->key, P_STATIC_STRING("!~"), 2);
		return true;
	}

	void insertCurrentHeader() {
		if (!state->secureMode) {
			message->headers.insert(&state->currentHeader, pool);
		} else {
			message->secureHeaders.insert(&state->currentHeader, pool);
		}
	}

	static size_t http_parser_execute_and_handle_pause(http_parser *parser,
		const http_parser_settings *settings, const char *data, size_t len,
		bool &paused)
	{
		size_t ret = http_parser_execute(parser, settings, data, len);
		paused = len > 0 && ret != len && HTTP_PARSER_ERRNO(parser) == HPE_PAUSED;
		if (paused) {
			http_parser_pause(parser, 0);
			http_parser_execute(parser, settings, data + len - 1, 1);
		}
		return ret;
	}

	static int _onURL(http_parser *parser, const char *data, size_t len) {
		HttpHeaderParser *self = static_cast<HttpHeaderParser *>(parser->data);
		return self->onURL(MessageType(), data, len);
	}

	OXT_FORCE_INLINE
	int onURL(const HttpParseRequest &tag, const char *data, size_t len) {
		state->state = HttpHeaderParserState::PARSING_URL;
		psg_lstr_append(&message->path, pool, *currentBuffer, data, len);
		return 0;
	}

	OXT_FORCE_INLINE
	int onURL(const HttpParseResponse &tag, const char *data, size_t len) {
		P_BUG("Should never be called");
		return 0;
	}

	static int onStatus(http_parser *parser, const char *data, size_t len) {
		HttpHeaderParser *self = static_cast<HttpHeaderParser *>(parser->data);
		if (parser->status_code == 100) {
			self->set100ContinueHttpState(MessageType());
			http_parser_pause(parser, 1);
		}
		return 0;
	}

	void set100ContinueHttpState(const HttpParseRequest &tag) {
		P_BUG("Should never be called");
	}

	void set100ContinueHttpState(const HttpParseResponse &tag) {
		message->httpState = Message::ONEHUNDRED_CONTINUE;
	}

	static int onHeaderField(http_parser *parser, const char *data, size_t len) {
		HttpHeaderParser *self = static_cast<HttpHeaderParser *>(parser->data);

		if (self->state->state == HttpHeaderParserState::PARSING_NOT_STARTED
		 || self->state->state == HttpHeaderParserState::PARSING_URL
		 || self->state->state == HttpHeaderParserState::PARSING_HEADER_VALUE
		 || self->state->state == HttpHeaderParserState::PARSING_FIRST_HEADER_VALUE)
		{
			// New header field encountered.

			if (self->state->state == HttpHeaderParserState::PARSING_FIRST_HEADER_VALUE
			 || self->state->state == HttpHeaderParserState::PARSING_HEADER_VALUE)
			{
				// Validate previous header and insert into table.
				if (!self->validateHeader(MessageType(), self->state->currentHeader)) {
					return 1;
				}
				self->insertCurrentHeader();
			}

			// Initialize new header field.
			self->state->currentHeader = (Header *) psg_palloc(self->pool, sizeof(Header));
			psg_lstr_init(&self->state->currentHeader->key);
			psg_lstr_init(&self->state->currentHeader->origKey);
			psg_lstr_init(&self->state->currentHeader->val);
			self->state->hasher.reset();
			if (self->state->state == HttpHeaderParserState::PARSING_URL) {
				self->state->state = HttpHeaderParserState::PARSING_FIRST_HEADER_FIELD;
			} else {
				self->state->state = HttpHeaderParserState::PARSING_HEADER_FIELD;
			}
		}

		psg_lstr_append(&self->state->currentHeader->origKey, self->pool,
			*self->currentBuffer, data, len);
		if (psg_lstr_first_byte(&self->state->currentHeader->origKey) == '!') {
			psg_lstr_append(&self->state->currentHeader->key, self->pool,
				*self->currentBuffer, data, len);
			self->state->hasher.update(data, len);
		} else {
			char *downcasedData = (char *) psg_pnalloc(self->pool, len);
			convertLowerCase((const unsigned char *) data,
				(unsigned char *) downcasedData, len);
			psg_lstr_append(&self->state->currentHeader->key, self->pool,
				downcasedData, len);
			self->state->hasher.update(downcasedData, len);
		}

		return 0;
	}

	static int onHeaderValue(http_parser *parser, const char *data, size_t len) {
		HttpHeaderParser *self = static_cast<HttpHeaderParser *>(parser->data);

		if (self->state->state == HttpHeaderParserState::PARSING_FIRST_HEADER_FIELD
		 || self->state->state == HttpHeaderParserState::PARSING_HEADER_FIELD)
		{
			// New header value encountered. Finalize corresponding header field.
			if (self->state->state == HttpHeaderParserState::PARSING_FIRST_HEADER_FIELD) {
				self->state->state = HttpHeaderParserState::PARSING_FIRST_HEADER_VALUE;
			} else {
				self->state->state = HttpHeaderParserState::PARSING_HEADER_VALUE;
			}
			self->state->currentHeader->hash = self->state->hasher.finalize();

		}

		psg_lstr_append(&self->state->currentHeader->val, self->pool,
			*self->currentBuffer, data, len);
		self->state->hasher.update(data, len);

		return 0;
	}

	static int onHeadersComplete(http_parser *parser) {
		HttpHeaderParser *self = static_cast<HttpHeaderParser *>(parser->data);

		if (self->state->state == HttpHeaderParserState::PARSING_HEADER_VALUE
		 || self->state->state == HttpHeaderParserState::PARSING_FIRST_HEADER_VALUE)
		{
			// Validate previous header and insert into table.
			if (!self->validateHeader(MessageType(), self->state->currentHeader)) {
				// There's a bug in http_parser: even if we return 1 here, it doesn't
				// set the error flag correctly. We fix that here.
				self->state->parser.http_errno = HPE_CB_headers_complete;
				return 1;
			}
			self->insertCurrentHeader();
		}

		self->state->currentHeader = NULL;
		self->message->httpState = Message::PARSED_HEADERS;
		self->indexQueryString(MessageType());
		http_parser_pause(parser, 1);
		return 0;
	}

	OXT_FORCE_INLINE
	void indexQueryString(const HttpParseRequest &tag) {
		LString *contiguousPath = psg_lstr_make_contiguous(&message->path,
			message->pool);
		if (contiguousPath != &message->path) {
			psg_lstr_deinit(&message->path);
			message->path = *contiguousPath;
		}

		const char *pos = (const char *) memchr(message->path.start->data, '?',
			message->path.size);
		if (pos != NULL) {
			message->queryStringIndex = pos - message->path.start->data;
		}
	}

	OXT_FORCE_INLINE
	void indexQueryString(const HttpParseResponse &tag) {
		// Do nothing.
	}

	OXT_FORCE_INLINE
	void initializeParser(const HttpParseRequest &tag) {
		http_parser_init(&state->parser, HTTP_REQUEST);
	}

	OXT_FORCE_INLINE
	void initializeParser(const HttpParseResponse &tag) {
		http_parser_init(&state->parser, HTTP_RESPONSE);
	}

	OXT_FORCE_INLINE
	bool messageHttpStateIndicatesCompletion(const HttpParseRequest &tag) const {
		return message->httpState == Message::PARSED_HEADERS;
	}

	OXT_FORCE_INLINE
	bool messageHttpStateIndicatesCompletion(const HttpParseResponse &tag) const {
		return message->httpState == Message::PARSED_HEADERS
			|| message->httpState == Message::ONEHUNDRED_CONTINUE;
	}

	void processParseResult(const HttpParseRequest &tag) {
		TRACE_POINT();
		bool isChunked = state->parser.flags & F_CHUNKED;
		boost::uint64_t contentLength;
		int httpVersion;

		// The parser sets content_length to ULLONG_MAX if
		// Content-Length is not given. We treat it the same as 0.
		contentLength = state->parser.content_length;
		if (contentLength == std::numeric_limits<boost::uint64_t>::max()) {
			contentLength = 0;
		}

		message->method = (http_method) state->parser.method;
		httpVersion = state->parser.http_major * 1000 + state->parser.http_minor * 10;

		if (httpVersion > 1010) {
			// Maximum supported HTTP version is 1.1
			message->httpState      = Message::ERROR;
			message->aux.parseError = HTTP_VERSION_NOT_SUPPORTED;
			message->httpMajor      = 1;
			message->httpMinor      = 1;
			message->wantKeepAlive  = false;
		} else if (contentLength > 0 && isChunked) {
			message->httpState  = Message::ERROR;
			message->aux.parseError = REQUEST_CONTAINS_CONTENT_LENGTH_AND_TRANSFER_ENCODING;
		} else if (contentLength > 0 || isChunked) {
			// There is a request body.
			message->aux.bodyInfo.contentLength = contentLength;
			if (state->parser.upgrade) {
				message->httpState      = Message::ERROR;
				message->aux.parseError = UPGRADE_NOT_ALLOWED_WHEN_REQUEST_BODY_EXISTS;
			} else if (isChunked) {
				message->httpState = Message::PARSING_CHUNKED_BODY;
				message->bodyType  = Message::RBT_CHUNKED;
			} else {
				message->httpState = Message::PARSING_BODY;
				message->bodyType  = Message::RBT_CONTENT_LENGTH;
			}
		} else {
			// There is no request body.
			if (!state->parser.upgrade) {
				message->httpState = Message::COMPLETE;
				P_ASSERT_EQ(message->bodyType, Message::RBT_NO_BODY);
			} else if (message->method != HTTP_HEAD) {
				message->httpState = Message::UPGRADED;
				message->bodyType  = Message::RBT_UPGRADE;
				message->wantKeepAlive = false;
			} else {
				message->httpState      = Message::ERROR;
				message->aux.parseError = UPGRADE_NOT_ALLOWED_FOR_HEAD_REQUESTS;
			}
		}
	}

	void processParseResult(const HttpParseResponse &tag) {
		TRACE_POINT();
		const unsigned int status = state->parser.status_code;
		const boost::uint64_t contentLength = state->parser.content_length;

		message->statusCode = state->parser.status_code;

		if (state->parser.upgrade) {
			message->httpState = Message::UPGRADED;
			message->bodyType  = Message::RBT_UPGRADE;
			message->wantKeepAlive = false;
		} else if (message->headers.lookup(HTTP_X_SENDFILE) != NULL
		 || message->headers.lookup(HTTP_X_ACCEL_REDIRECT) != NULL)
		{
			// If X-Sendfile or X-Accel-Redirect is set, pretend like the body
			// is empty and disallow keep-alive. See:
			// https://github.com/phusion/passenger/issues/1376
			// https://github.com/phusion/passenger/issues/1498
			//
			// We don't set a fake "Content-Length: 0" header here
			// because it's undefined what Content-Length means if
			// X-Sendfile or X-Accel-Redirect are set.
			//
			// Because the response header no longer has any header
			// that signals its size, keep-alive should also be disabled
			// for the *request*. We already do that in RequestHandler's
			// ForwardResponse.cpp.
			message->httpState = Message::COMPLETE;
			message->bodyType = Message::RBT_NO_BODY;
			message->headers.erase(HTTP_CONTENT_LENGTH);
			message->headers.erase(HTTP_TRANSFER_ENCODING);
			message->wantKeepAlive = false;
		} else if (requestMethod == HTTP_HEAD
		 || status / 100 == 1  // status 1xx
		 || status == 204
		 || status == 304)
		{
			if (status != 100) {
				message->httpState = Message::COMPLETE;
			} else {
				message->httpState = Message::ONEHUNDRED_CONTINUE;
			}
			message->bodyType = Message::RBT_NO_BODY;
		} else if (state->parser.flags & F_CHUNKED) {
			if (contentLength == std::numeric_limits<boost::uint64_t>::max()) {
				message->httpState = Message::PARSING_CHUNKED_BODY;
				message->bodyType  = Message::RBT_CHUNKED;
			} else {
				message->httpState      = Message::ERROR;
				message->aux.parseError = RESPONSE_CONTAINS_CONTENT_LENGTH_AND_TRANSFER_ENCODING;
			}
		} else if (contentLength == 0) {
			message->httpState = Message::COMPLETE;
			message->bodyType  = Message::RBT_NO_BODY;
		} else if (contentLength != std::numeric_limits<boost::uint64_t>::max()) {
			message->httpState = Message::PARSING_BODY_WITH_LENGTH;
			message->bodyType  = Message::RBT_CONTENT_LENGTH;
			message->aux.bodyInfo.contentLength = contentLength;
		} else {
			message->httpState = Message::PARSING_BODY_UNTIL_EOF;
			message->bodyType  = Message::RBT_UNTIL_EOF;
			message->wantKeepAlive = false;
		}
	}

public:
	HttpHeaderParser(Context *context, HttpHeaderParserState *_state,
		Message *_message, psg_pool_t *_pool,
		enum http_method _requestMethod = HTTP_GET)
		: ctx(context),
		  state(_state),
		  message(_message),
		  pool(_pool),
		  currentBuffer(NULL),
		  requestMethod(_requestMethod)
		{ }

	void initialize() {
		initializeParser(MessageType());
		state->state = HttpHeaderParserState::PARSING_NOT_STARTED;
		state->secureMode = false;
	}

	size_t feed(const MemoryKit::mbuf &buffer) {
		TRACE_POINT();
		P_ASSERT_EQ(message->httpState, Message::PARSING_HEADERS);

		http_parser_settings settings;
		size_t ret;
		bool paused;

		settings.on_message_begin = NULL;
		settings.on_url = _onURL;
		settings.on_status = onStatus;
		settings.on_header_field = onHeaderField;
		settings.on_header_value = onHeaderValue;
		settings.on_headers_complete = onHeadersComplete;
		settings.on_body = NULL;
		settings.on_message_complete = NULL;

		state->parser.data = this;
		currentBuffer = &buffer;
		ret = http_parser_execute_and_handle_pause(&state->parser,
			&settings, buffer.start, buffer.size(), paused);
		currentBuffer = NULL;

		if (!state->parser.upgrade && ret != buffer.size() && !paused) {
			UPDATE_TRACE_POINT();
			message->httpState = Message::ERROR;
			switch (HTTP_PARSER_ERRNO(&state->parser)) {
			case HPE_CB_header_field:
			case HPE_CB_headers_complete:
				switch (state->state) {
				case HttpHeaderParserState::ERROR_SECURITY_PASSWORD_MISMATCH:
					message->aux.parseError = SECURITY_PASSWORD_MISMATCH;
					break;
				case HttpHeaderParserState::ERROR_SECURITY_PASSWORD_DUPLICATE:
					message->aux.parseError = SECURITY_PASSWORD_DUPLICATE;
					break;
				case HttpHeaderParserState::ERROR_SECURE_HEADER_NOT_ALLOWED:
					message->aux.parseError = ERROR_SECURE_HEADER_NOT_ALLOWED;
					break;
				case HttpHeaderParserState::ERROR_NORMAL_HEADER_NOT_ALLOWED_AFTER_SECURITY_PASSWORD:
					message->aux.parseError = NORMAL_HEADER_NOT_ALLOWED_AFTER_SECURITY_PASSWORD;
					break;
				default:
					goto default_error;
				}
				break;
			default:
				default_error:
				message->aux.parseError = HTTP_PARSER_ERRNO_BEGIN - HTTP_PARSER_ERRNO(&state->parser);
				break;
			}
		} else if (messageHttpStateIndicatesCompletion(MessageType())) {
			UPDATE_TRACE_POINT();
			ret++;
			message->httpMajor = state->parser.http_major;
			message->httpMinor = state->parser.http_minor;
			message->wantKeepAlive = http_should_keep_alive(&state->parser);
			processParseResult(MessageType());
		}

		return ret;
	}
};


} // namespace ServerKit
} // namespace Passenger

#endif /* _PASSENGER_SERVER_KIT_HTTP_HEADER_PARSER_H_ */
