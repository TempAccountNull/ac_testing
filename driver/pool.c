#include "pool.h"

#include "common.h"

#include <intrin.h>

PKDDEBUGGER_DATA64 GetGlobalDebuggerData()
{
	CONTEXT context = { 0 };
	PDUMP_HEADER dump_header = { 0 };
	UINT64 thread_state;
	PKDDEBUGGER_DATA64 debugger_data = NULL;

	context.ContextFlags = CONTEXT_FULL;

	RtlCaptureContext( &context );

	dump_header = ExAllocatePool2( POOL_FLAG_NON_PAGED, DUMP_BLOCK_SIZE, POOL_DUMP_BLOCK_TAG );

	if ( !dump_header )
		goto end;

	KeCapturePersistentThreadState(
		&context,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		dump_header
	);

	debugger_data = ( PKDDEBUGGER_DATA64 )ExAllocatePool2( POOL_FLAG_NON_PAGED, sizeof( KDDEBUGGER_DATA64 ), POOL_DEBUGGER_DATA_TAG );

	if ( !debugger_data )
		goto end;

	RtlCopyMemory( debugger_data, dump_header->KdDebuggerDataBlock,  sizeof( KDDEBUGGER_DATA64 ));

end:

	if ( dump_header )
		ExFreePoolWithTag( dump_header, POOL_DUMP_BLOCK_TAG );

	return debugger_data;
}

VOID ScanPageForProcessAllocations(
	_In_ UINT64 PageBase,
	_In_ ULONG PageSize
)
{
	CHAR process[] = "\x50\x72\x6f\x63";
	INT length = strlen( process );

	if ( !PageBase || !PageSize )
		return;

	PAGED_CODE();

	for ( INT offset = 0; offset <= PageSize - length; offset++ )
	{
		for ( INT sig_index = 0; sig_index < length + 1; sig_index++ )
		{
			if ( !MmIsAddressValid( PageBase + offset + sig_index ) )
				break;

			CHAR current_char = *( PCHAR )( PageBase + offset + sig_index );
			CHAR current_sig_byte = process[ sig_index ];

			if ( sig_index == length )
			{
				PPOOL_HEADER pool_header = ( UINT64 )PageBase + offset - 0x04;

				if ( !MmIsAddressValid( (PVOID)pool_header ) )
					break;

				if ( pool_header->BlockSize * CHUNK_SIZE - sizeof(POOL_HEADER) == WIN_PROCESS_ALLOCATION_SIZE )
				{
					DEBUG_LOG( "prolly found proc: %llx", (UINT64)pool_header + sizeof(POOL_HEADER) );
				}

				break;
			}

			if ( current_char != current_sig_byte )
				break;
		}
	}
}

/*
* Using MmGetPhysicalMemoryRangesEx2(), we can get a block of structures that
* describe the physical memory layout. With each physical page base we are going
* to enumerate, we want to make sure it lies within an appropriate region of 
* physical memory, so this function is to check for exactly that.
*/
BOOLEAN IsPhysicalAddressInPhysicalMemoryRange(
	_In_ UINT64 PhysicalAddress,
	_In_ PPHYSICAL_MEMORY_RANGE PhysicalMemoryRanges
)
{
	ULONG page_index = 0;

	while ( PhysicalMemoryRanges[ page_index ].NumberOfBytes.QuadPart != NULL )
	{
		UINT64 start_address = PhysicalMemoryRanges[ page_index ].BaseAddress.QuadPart;
		UINT64 end_address = start_address + PhysicalMemoryRanges[ page_index ].NumberOfBytes.QuadPart;

		if ( PhysicalAddress >= start_address && PhysicalAddress <= end_address )
			return TRUE;

		page_index++;
	}

	return FALSE;
}

/*
* This is your basic page table walk function. On intel systems, paging has 4 levels,
* each table holds 512 entries with a total size of 0x1000 (512 * sizeof(QWORD)). Each entry
* in each table contains a value with a subset bitfield containing the physical address
* of the base of the next table in the structure. So for example, a PML4 entry contains
* a physical address that points to the base of the PDPT table, it is the same for a PDPT
* entry -> PD base and so on.
* 
* However, as with all good things Windows has implemented security features meaning 
* we cannot use functions such as MmCopyMemory or MmMapIoSpace on paging structures, 
* so we must find another way to walk the pages. Luckily for us, there exists 
* MmGetVirtualForPhysical. This function is self explanatory and returns the corresponding
* virtual address given a physical address. What this means is that we can extract a page
* entry physical address, pass it to MmGetVirtualForPhysical which returns us the virtual
* address of the base of the next page structure. This is because page tables are still 
* mapped by the kernel and exist in virtual memory just like everything else and hence
* reading the value at all 512 entries from the virtual base will give us the equivalent 
* value as directly reading the physical address.
* 
* Using this, we essentially walk the page tables as any regular translation would
* except instead of simply reading the physical we translate it to a virtual address
* and extract the physical address from the value at each virtual address page entry.
*/

VOID WalkKernelPageTables()
{
	CR3 cr3;
	PML4E pml4_base;
	PML4E pml4_entry;
	PDPTE pdpt_base;
	PDPTE pdpt_entry;
	PDPTE_LARGE pdpt_large_entry;
	PDE pd_base;
	PDE pd_entry;
	PDE_LARGE pd_large_entry;
	PTE pt_base;
	PTE pt_entry;
	UINT64 base_physical_page;
	UINT64 base_virtual_page;
	PHYSICAL_ADDRESS physical;
	PPHYSICAL_MEMORY_RANGE physical_memory_ranges;
	KIRQL irql;

	physical_memory_ranges = MmGetPhysicalMemoryRangesEx2( NULL, NULL );

	if ( physical_memory_ranges == NULL )
	{
		DEBUG_ERROR( "LOL stupid cunt not working" );
		return;
	}

	/* raise our irql to ensure we arent preempted by NOOB threads */
	KeRaiseIrql( DISPATCH_LEVEL, &irql );

	/* disable interrupts to prevent any funny business occuring */
	_disable();

	cr3.BitAddress = __readcr3();

	physical.QuadPart = cr3.Bits.PhysicalAddress << PAGE_4KB_SHIFT;

	pml4_base.BitAddress = MmGetVirtualForPhysical( physical );

	if ( !MmIsAddressValid( pml4_base.BitAddress ) || !pml4_base.BitAddress )
		return;

	for ( INT pml4_index = 0; pml4_index < PML4_ENTRY_COUNT; pml4_index++ )
	{
		pml4_entry.BitAddress = *(UINT64*)( pml4_base.BitAddress + pml4_index * sizeof( UINT64 ) );

		if ( pml4_entry.Bits.Present == NULL )
			continue;

		physical.QuadPart = pml4_entry.Bits.PhysicalAddress << PAGE_4KB_SHIFT;

		pdpt_base.BitAddress = MmGetVirtualForPhysical( physical );

		if ( !pdpt_base.BitAddress || !MmIsAddressValid( pdpt_base.BitAddress ) )
			continue;

		for ( INT pdpt_index = 0; pdpt_index < PDPT_ENTRY_COUNT; pdpt_index++ )
		{
			pdpt_entry.BitAddress = *( UINT64* )( pdpt_base.BitAddress + pdpt_index * sizeof( UINT64 ) );

			if ( pdpt_entry.Bits.Present == NULL )
				continue;

			if ( IS_LARGE_PAGE( pdpt_entry.BitAddress ) )
			{
				/* 2GB size page */
				pdpt_large_entry.BitAddress = pdpt_entry.BitAddress;
				continue;
			}

			physical.QuadPart = pdpt_entry.Bits.PhysicalAddress << PAGE_4KB_SHIFT;

			pd_base.BitAddress = MmGetVirtualForPhysical( physical );

			if ( !pd_base.BitAddress || !MmIsAddressValid( pd_base.BitAddress ) )
				continue;

			for ( INT pd_index = 0; pd_index < PD_ENTRY_COUNT; pd_index++ )
			{
				pd_entry.BitAddress = *( UINT64* )( pd_base.BitAddress + pd_index * sizeof( UINT64 ) );

				if ( pd_entry.Bits.Present == NULL )
					continue;

				if ( IS_LARGE_PAGE( pd_entry.BitAddress ) )
				{
					/* 2MB size page */
					pd_large_entry.BitAddress = pd_entry.BitAddress;
					continue;
				}

				physical.QuadPart = pd_entry.Bits.PhysicalAddress << PAGE_4KB_SHIFT;

				pt_base.BitAddress = MmGetVirtualForPhysical( physical );

				if ( !pt_base.BitAddress || !MmIsAddressValid( pt_base.BitAddress ) )
					continue;

				for ( INT pt_index = 0; pt_index < PT_ENTRY_COUNT; pt_index++ )
				{
					pt_entry.BitAddress = *( UINT64* )( pt_base.BitAddress + pt_index * sizeof( UINT64 ) );

					if ( pt_entry.Bits.Present == NULL )
						continue;

					physical.QuadPart = pt_entry.Bits.PhysicalAddress << PAGE_4KB_SHIFT;

					/* if the page base isnt in a legit region, go next */
					if ( IsPhysicalAddressInPhysicalMemoryRange( physical.QuadPart, physical_memory_ranges ) == FALSE )
						continue;

					base_virtual_page = MmGetVirtualForPhysical( physical );

					/* stupid fucking intellisense error GO AWAY! */
					if ( base_virtual_page == NULL || !MmIsAddressValid( base_virtual_page ) )
						continue;

					ScanPageForProcessAllocations( base_virtual_page, PAGE_BASE_SIZE );
				}
			}
		}
	}

	_enable();

	KeLowerIrql( irql );

	DEBUG_LOG( "Finished scanning memory" );

}

VOID ScanNonPagedPoolForProcessTags()
{
	NTSTATUS status;
	PKDDEBUGGER_DATA64 debugger_data = NULL;
	UINT64 non_paged_pool_start = NULL;
	UINT64 non_paged_pool_end = NULL;

	/* must free this */
	debugger_data = GetGlobalDebuggerData();

	if ( debugger_data == NULL )
	{
		DEBUG_ERROR( "Debugger data is null" );
		return STATUS_ABANDONED;
	}

	non_paged_pool_start = debugger_data->MmNonPagedPoolStart;
	non_paged_pool_end = debugger_data->MmNonPagedPoolEnd;

	DEBUG_LOG( "NonPagedPool start: %llx, end %llx", non_paged_pool_start, non_paged_pool_end );

	WalkKernelPageTables();

/*	for ( ; non_paged_pool_start <= non_paged_pool_end; non_paged_pool_start++ )
	{
		CHAR current_byte = *( CHAR* )non_paged_pool_start;
		DEBUG_LOG( "Current byte: %c", current_byte );
	*/

	ExFreePoolWithTag( debugger_data, POOL_DEBUGGER_DATA_TAG );
}