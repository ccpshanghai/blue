// Copyright © 2026 CCP ehf.
//
// Blue C API: resource waits (1.2). A wait is a native IBlueAsyncResNotifyTarget that records
// how a load ended and hands it to the host from blue_pump (or blue_shutdown), outside blue's
// notify loop: BlueAsyncRes::NotifyRebuildCachedData calls the targets in a loop that a
// target removed, or a resource released, inside it would break.

#include "StdAfx.h"

#if BLUE_WITH_CABI

#include "BlueCApiInternal.h"
#include "BlueAsyncRes.h"

#include <algorithm>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace BlueCApi;

namespace
{

class Waiter final : public IBlueAsyncResNotifyTarget
{
public:
	Waiter( uint64_t id, BlueResourceWaitFn fn, void* userData, BlueAsyncRes* resource )
		: m_id( id )
		, m_fn( fn )
		, m_userData( userData )
		, m_resource( resource )
	{
	}

	uint64_t Id() const
	{
		return m_id;
	}

	// Record the outcome (the first one wins) and queue the waiter for delivery
	void Decide( int32_t state );

	// From the resource's own state, for a resource that is prepared already
	void DecideFromResource()
	{
		RebuildCachedData( m_resource );
	}

	void Register()
	{
		m_resource->AddNotifyTarget( this );
		m_registered = true;
	}

	// Remove the notify target, run fn and drop the resource reference, in that order: a
	// BlueAsyncRes must have no targets left when it is destroyed. pendingState stands in for
	// a state not decided yet (SHUTDOWN).
	void Deliver( int32_t pendingState )
	{
		if( m_registered )
		{
			m_registered = false;
			m_resource->RemoveNotifyTarget( this );
		}
		const int32_t state = m_state ? m_state : pendingState;
		try
		{
			m_fn( m_userData, state );
		}
		catch( ... )
		{
			// A host function must not throw; whatever it did, nothing crosses the C API
		}
		m_resource.Unlock();
	}

protected:
	// Called inside BlueAsyncRes::NotifyRebuildCachedData's loop: only record
	void RebuildCachedData( BlueAsyncRes* resource ) override
	{
		if( resource->IsPrepared() )
		{
			Decide( resource->IsGood() ? BLUE_RESOURCE_READY : BLUE_RESOURCE_FAILED );
		}
	}

	// A Reload releases the cached data, then loads again and notifies again
	void ReleaseCachedData( BlueAsyncRes* ) override
	{
	}

private:
	uint64_t m_id;
	BlueResourceWaitFn m_fn;
	void* m_userData;
	BlueAsyncResPtr m_resource;		// one reference until delivered
	bool m_registered = false;		// AddNotifyTarget done, RemoveNotifyTarget pending
	bool m_due = false;				// in Due()
	int32_t m_state = 0;			// a BlueResourceState once decided, 0 while pending
};

typedef std::unordered_map<uint64_t, std::unique_ptr<Waiter>> Waiters;

// Main thread only (every entry point checks the caller). Allocated once and never destroyed:
// a host may exit without blue_shutdown, and destroying waiters at exit would release blue
// objects after blue's own globals are gone. LeakSanitizer reaches them through the pointer.
Waiters& AllWaiters()
{
	static Waiters* s_waiters = new Waiters;
	return *s_waiters;
}

// Ids of decided waiters, in the order they were decided
std::vector<uint64_t>& Due()
{
	static std::vector<uint64_t>* s_due = new std::vector<uint64_t>;
	return *s_due;
}

uint64_t s_nextId = 0;
bool s_delivering = false;

void Waiter::Decide( int32_t state )
{
	if( m_state == 0 )
	{
		m_state = state;
	}
	if( !m_due )
	{
		m_due = true;
		Due().push_back( m_id );
	}
}

// Take waiter id out of the registry and deliver it (pendingState for one still pending)
void TakeAndDeliver( uint64_t id, int32_t pendingState )
{
	Waiters& waiters = AllWaiters();
	auto found = waiters.find( id );
	if( found == waiters.end() )
	{
		// Delivered already (by a blue_shutdown inside an earlier fn)
		return;
	}
	std::unique_ptr<Waiter> waiter = std::move( found->second );
	waiters.erase( found );
	try
	{
		waiter->Deliver( pendingState );
	}
	catch( ... )
	{
		// Keep delivering the others
	}
}

} // namespace


namespace BlueCApi
{

void DeliverResourceWaits()
{
	if( s_delivering )
	{
		return;
	}
	s_delivering = true;
	// The ones decided so far; a wait decided while these run waits for the next pump
	std::vector<uint64_t> due;
	due.swap( Due() );
	for( uint64_t id : due )
	{
		TakeAndDeliver( id, 0 );
	}
	s_delivering = false;
}

void ShutDownResourceWaits()
{
	// Decided ones first, in their order, then the rest by id: the pending ones get SHUTDOWN.
	// (The rest can hold decided ones too, when blue_shutdown runs inside an fn that
	// blue_pump delivers.)
	std::vector<uint64_t> ids;
	ids.swap( Due() );
	std::vector<uint64_t> rest;
	for( const auto& entry : AllWaiters() )
	{
		if( std::find( ids.begin(), ids.end(), entry.first ) == ids.end() )
		{
			rest.push_back( entry.first );
		}
	}
	std::sort( rest.begin(), rest.end() );
	ids.insert( ids.end(), rest.begin(), rest.end() );

	const bool wasDelivering = s_delivering;
	s_delivering = true;
	for( uint64_t id : ids )
	{
		TakeAndDeliver( id, BLUE_RESOURCE_SHUTDOWN );
	}
	s_delivering = wasDelivering;
}

} // namespace BlueCApi


int32_t blue_resource_wait( BlueObject* resource, BlueResourceWaitFn fn, void* user_data, uint64_t* out_id, BlueCError* err )
{
	if( out_id )
	{
		*out_id = 0;
	}
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		if( !resource || !fn || !out_id )
		{
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "blue_resource_wait needs a resource, a function and an out pointer" );
		}
		IRoot* root = reinterpret_cast<IRoot*>( resource );
		// QueryInterface walks the class and its parents, as BlueCastPtr does everywhere in blue
		BlueAsyncResPtr asyncRes( BlueCastPtr( root ) );
		if( !asyncRes )
		{
			return BlueCSetError( &error, BLUE_E_TYPE, "object of class %s is not a BlueAsyncRes", BlueCClassNameOf( root ) );
		}
		const bool prepared = asyncRes->IsPrepared();
		if( !prepared && !asyncRes->IsLoading() )
		{
			// Such a resource has no path, unless a subclass gave it one without loading
			std::string path;
			const wchar_t* widePath = asyncRes->GetPath();
			if( widePath && *widePath && BlueCWideToUtf8( widePath, wcslen( widePath ), path ) )
			{
				return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "resource '%s' is neither loading nor loaded (it was never initialised)", path.c_str() );
			}
			return BlueCSetError( &error, BLUE_E_INVALID_ARGUMENT, "the %s is neither loading nor loaded (it was never initialised)", BlueCClassNameOf( root ) );
		}

		std::unique_ptr<Waiter> waiter( new Waiter( ++s_nextId, fn, user_data, asyncRes ) );
		if( prepared )
		{
			// Finished already: delivered from the next blue_pump, without a notify target
			waiter->DecideFromResource();
		}
		else
		{
			// Notifies at once only for a prepared resource, which this one is not
			waiter->Register();
		}
		*out_id = waiter->Id();
		AllWaiters().emplace( waiter->Id(), std::move( waiter ) );
		return BLUE_OK;
	} );
}

int32_t blue_resource_wait_cancel( uint64_t id, BlueCError* err )
{
	return Call( err, [&]( BlueCError& error ) -> int32_t {
		Waiters& waiters = AllWaiters();
		auto found = waiters.find( id );
		if( found == waiters.end() )
		{
			return BlueCSetError( &error, BLUE_E_NOT_FOUND, "no resource wait %llu (unknown, or delivered already)", (unsigned long long)id );
		}
		// A decided waiter keeps its state; either way blue_pump delivers it, so no notify
		// list is edited here (this may run inside another notification)
		found->second->Decide( BLUE_RESOURCE_CANCELLED );
		return BLUE_OK;
	} );
}

#endif
