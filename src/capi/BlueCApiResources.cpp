// Copyright © 2026 CCP ehf.
//
// Blue C API: resource waits (1.2). A wait is a native IBlueAsyncResNotifyTarget that records
// how a load ended and hands it to the host from blue_pump (or blue_shutdown), outside blue's
// notify loop: BlueAsyncRes::NotifyRebuildCachedData calls the targets in a loop that a
// target removed, or a resource released, inside it would break.
//
// The notification is not the only way a wait is decided (1.3): blue does not notify a load
// whose DoLoad returns LR_SUCCESS_DONTNOTIFY, nor one CancelPendingLoad abandons. So each
// delivery first reads the state of every resource still waited for, and a wait decided
// READY or FAILED whose resource is loading again (a Reload since) waits for that load.

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

	// blue_resource_wait_cancel: CANCELLED unless decided already
	void Cancel()
	{
		m_cancelled = true;
		Decide( BLUE_RESOURCE_CANCELLED );
	}

	bool IsDecided() const
	{
		return m_state != 0;
	}

	// For a wait not decided yet, from the resource's state: nothing while it loads, READY or
	// FAILED once it is prepared, FAILED when it is neither (a load blue abandoned)
	void Poll()
	{
		if( m_state != 0 || m_resource->IsLoading() )
		{
			return;
		}
		if( m_resource->IsPrepared() )
		{
			Decide( m_resource->IsGood() ? BLUE_RESOURCE_READY : BLUE_RESOURCE_FAILED );
		}
		else
		{
			Decide( BLUE_RESOURCE_FAILED );
		}
	}

	// Decided READY or FAILED, not cancelled, and the resource is loading again (a Reload
	// since the decision): that load is waited for
	bool IsLoadingAgain() const
	{
		return IsLoadEnd( m_state ) && !m_cancelled && m_resource->IsLoading();
	}

	// Back to pending, out of Due() (the caller took it out): decided again by a notification,
	// when it is a notify target, or by the poll of a later delivery
	void Undecide()
	{
		m_state = 0;
		m_due = false;
	}

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
		int32_t state = m_state ? m_state : pendingState;
		if( IsLoadEnd( state ) && m_resource->IsLoading() )
		{
			// Loading again since it was decided, and ended first by a cancel or by blue_shutdown
			if( m_cancelled )
			{
				state = BLUE_RESOURCE_CANCELLED;
			}
			else if( pendingState )
			{
				state = pendingState;
			}
		}
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
	static bool IsLoadEnd( int32_t state )
	{
		return state == BLUE_RESOURCE_READY || state == BLUE_RESOURCE_FAILED;
	}

	uint64_t m_id;
	BlueResourceWaitFn m_fn;
	void* m_userData;
	BlueAsyncResPtr m_resource;		// one reference until delivered
	bool m_registered = false;		// AddNotifyTarget done, RemoveNotifyTarget pending
	bool m_due = false;				// in Due()
	bool m_cancelled = false;		// blue_resource_wait_cancel called
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

// Decide every wait not decided yet whose resource is no longer loading, in id order
void PollWaits()
{
	std::vector<Waiter*> undecided;
	for( const auto& entry : AllWaiters() )
	{
		if( !entry.second->IsDecided() )
		{
			undecided.push_back( entry.second.get() );
		}
	}
	std::sort( undecided.begin(), undecided.end(), []( const Waiter* a, const Waiter* b ) { return a->Id() < b->Id(); } );
	for( Waiter* waiter : undecided )
	{
		waiter->Poll();
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
	try
	{
		PollWaits();
	}
	catch( ... )
	{
		// Out of memory: the notified waits are still delivered; the rest at the next pump
	}
	// The ones decided so far; a wait decided while these run waits for the next pump
	std::vector<uint64_t> due;
	due.swap( Due() );
	Waiters& waiters = AllWaiters();
	for( uint64_t id : due )
	{
		auto found = waiters.find( id );
		if( found != waiters.end() && found->second->IsLoadingAgain() )
		{
			// Reloaded since it was decided: stays in the registry for that load
			found->second->Undecide();
			continue;
		}
		TakeAndDeliver( id, 0 );
	}
	s_delivering = false;
}

void ShutDownResourceWaits()
{
	// Decided ones first, in their order, then the rest by id: the pending ones get SHUTDOWN.
	// (The rest can hold decided ones too, when blue_shutdown runs inside an fn that
	// blue_pump delivers.) A load that ended without a notification counts as decided.
	try
	{
		PollWaits();
	}
	catch( ... )
	{
		// Out of memory: the ones not polled end as SHUTDOWN
	}
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

		// Into the registry before the resource knows the waiter, so a failure (out of memory)
		// leaves no notify target behind for a destroyed waiter
		const uint64_t id = ++s_nextId;
		Waiters& waiters = AllWaiters();
		Waiter& waiter = *waiters.emplace( id, std::unique_ptr<Waiter>( new Waiter( id, fn, user_data, asyncRes ) ) ).first->second;
		try
		{
			if( prepared )
			{
				// Finished already: delivered from the next blue_pump, without a notify target
				waiter.DecideFromResource();
			}
			else
			{
				// Notifies at once only for a prepared resource, which this one is not
				waiter.Register();
			}
		}
		catch( ... )
		{
			// AddNotifyTarget throws before it adds the target, so only our own entries remain
			Due().erase( std::remove( Due().begin(), Due().end(), id ), Due().end() );
			waiters.erase( id );
			throw;
		}
		*out_id = id;
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
		found->second->Cancel();
		return BLUE_OK;
	} );
}

#endif
