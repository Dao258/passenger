/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2010 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  See LICENSE file for license information.
 */

#ifndef _PASSENGER_HASH_MAP_H_
#define _PASSENGER_HASH_MAP_H_

/*
 * There are too many ways to include hash_map/unordered_map!
 * This header autodetects the correct method.
 */
#if defined(HAS_TR1_UNORDERED_MAP)
	#include <tr1/unordered_map>
	#define HashMap std::tr1::unordered_map
#elif defined(HASH_NAMESPACE) && defined(HASH_MAP_HEADER)
	#include HASH_MAP_HEADER
	#define HashMap HASH_NAMESPACE::hash_map
#elif defined(__GNUC__)
	#include <ext/hash_map>
	#define HashMap __gnu_cxx::hash_map
#elif defined(_MSC_VER)
	#include <hash_map>
	#define HashMap stdext::hash_map
#else
	#include <tr/unordered_map>
	#define HashMap std::tr1::unordered_map;
#endif

#endif /* _PASSENGER_HASH_MAP_H_ */
