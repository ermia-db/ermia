#pragma once

#include <cstring>
#include <atomic>
#include <cassert>
#include "macros.h"
#include "dbcore/dynarray.h"
#include <sched.h>
#include <numa.h>
#include <limits>
#include "dbcore/sm-common.h"

#define NR_SOCKETS 4
// each socket requests this many oids a time from global alloc
#define OID_EXT_SIZE 8192

typedef unsigned long long oid_type;

struct dynarray;

// this really shound't live here...
#define RA_NUM_SEGMENTS 4

class object
{
	public:
		object( size_t size ) : _size(size) { _next = fat_ptr::make( (void*)0, INVALID_SIZE_CODE); }
		inline char* payload() { return (char*)((char*)this + sizeof(object)); }

		fat_ptr _next;
		size_t _size;			// contraint on object size( practical enough )
};

template <typename T>
class object_vector
{
public:
	inline unsigned long long size() 
	{
		return _global_oid_alloc_offset + 1;
    }

	object_vector( unsigned long long nelems)
	{
        _global_oid_alloc_offset = 0;
		_obj_table = dynarray(  std::numeric_limits<unsigned int>::max() * sizeof(fat_ptr), nelems*sizeof(fat_ptr) );

        for (uint i = 0 ; i < RA_NUM_SEGMENTS; i++)
            _temperature_bitmap[i] = dynarray(std::numeric_limits<unsigned int>::max(), _obj_table.size() / sizeof(fat_ptr) / _oids_per_byte);
	}

	bool put( oid_type oid, fat_ptr new_head)
	{
//		ALWAYS_ASSERT( oid > 0 && oid <= _alloc_offset );
		fat_ptr old_head = begin(oid);
		object* new_desc = (object*)new_head.offset();
		volatile_write( new_desc->_next, old_head);
		uint64_t* p = (uint64_t*)begin_ptr(oid);

		if( not __sync_bool_compare_and_swap( p, old_head._ptr, new_head._ptr) )
			return false;

        // new record, shuold be in cold store, no need to change temp bit
		return true;
	}
	bool put( oid_type oid, fat_ptr old_head, fat_ptr new_head )
	{
//		ALWAYS_ASSERT( oid > 0 && oid <= _alloc_offset );
		object* new_desc = (object*)new_head.offset();
		volatile_write( new_desc->_next, old_head);
		uint64_t* p = (uint64_t*)begin_ptr(oid);

		if( not __sync_bool_compare_and_swap( p, old_head._ptr, new_head._ptr) )
			return false;

		return true;
	}

	inline fat_ptr begin( oid_type oid )
	{
        ASSERT(oid <= size());
		fat_ptr* ret = begin_ptr(oid);
		return volatile_read(*ret);
	}

    inline fat_ptr* begin_ptr(oid_type oid)
    {
        // tzwang: I guess we don't need volatile_read for this
        return (fat_ptr*)(&_obj_table[oid * sizeof(fat_ptr)]);
    }

	void unlink( oid_type oid, T item )
	{
		object* target;
		fat_ptr prev;
		fat_ptr* prev_next;
//		ALWAYS_ASSERT( oid > 0 && oid <= _alloc_offset );

retry:
		prev_next = begin_ptr( oid );			// constant value. doesn't need to be volatile_read
		prev= volatile_read(*prev_next);
		target = (object*)prev.offset();
		while( target )
		{
			if( target->payload() == (char*)item )
			{
				if( not __sync_bool_compare_and_swap( (uint64_t *)prev_next, prev._ptr, target->_next._ptr ) )
					goto retry;

				return;
			}
			prev_next = &target->_next;	// only can be modified by current TX. volatile_read is not needed
			prev = volatile_read(*prev_next);
			target = (object*)prev.offset();
		}

		if( !target )
			ALWAYS_ASSERT(false);
	}

	inline oid_type alloc()
	{
        if (_core_oid_remaining.my() == 0) {
            _core_oid_offset.my() = alloc_oid_extent();
            _core_oid_remaining.my() = OID_EXT_SIZE;
        }
        return _core_oid_offset.my() + OID_EXT_SIZE - (_core_oid_remaining.my()--) + 1;
    }

    inline uint64_t alloc_oid_extent() {
		uint64_t noffset = __sync_fetch_and_add(&_global_oid_alloc_offset, OID_EXT_SIZE);

		uint64_t obj_table_size = sizeof(fat_ptr) * (_global_oid_alloc_offset);
		_obj_table.ensure_size( obj_table_size + ( obj_table_size / 10) );			// 10% increase
        for (uint i = 0; i < RA_NUM_SEGMENTS; i++)
            _temperature_bitmap[i].ensure_size(_obj_table.size() / sizeof(fat_ptr) / _oids_per_byte);
        return noffset;
	}

    inline void set_temperature(oid_type oid, bool hot, fat_ptr fp)
    {
        set_temperature(oid, hot, fp.mem_segment());
    }

    inline void set_temperature(oid_type oid, bool hot, int seg)
    {
        ASSERT(seg < RA_NUM_SEGMENTS);
        uint64_t groupid = oid_group(oid);
        temp_bitmap_type gmap = volatile_read(_temperature_bitmap[seg][groupid]);
        temp_bitmap_type nmap = hot ?
            gmap | (uint64_t){1} << (groupid % sizeof(temp_bitmap_type)) :
            gmap & (~((uint64_t){1} << (groupid % sizeof(temp_bitmap_type))));
        if (gmap != nmap)
            __sync_bool_compare_and_swap(&_temperature_bitmap[seg][groupid], gmap, nmap);
    }

    inline uint64_t oid_group(oid_type oid)
    {
        return oid / _oids_per_word;
    }

    inline bool is_hot_group(uint64_t groupid, int gc_segment)
    {
        return _temperature_bitmap[gc_segment][groupid / sizeof(temp_bitmap_type)] &
               ((uint64_t{1} << (groupid % sizeof(temp_bitmap_type))));
    }

	inline uint64_t oid_group_sz()
	{
		return _oids_per_word;
	}

    typedef uint64_t temp_bitmap_type;

private:
	dynarray 		_obj_table;

    // each segment (i.e., gc cycle) has one bitmap for each gc daemon
	dynarray        _temperature_bitmap[RA_NUM_SEGMENTS];
	uint64_t _oids_per_bit = 8;
	uint64_t _oids_per_byte = 64;
	uint64_t _oids_per_word = 512;
    uint64_t _global_oid_alloc_offset;
    percore<uint64_t, false, false> _core_oid_offset;
    percore<uint64_t, false, false> _core_oid_remaining;
};
