#include "GameMemory.h"

#include <windows.h>

#include <cmath>

namespace GameMemory
{
	namespace
	{
		const Layout LAYOUTS[] = {
			// build 210545, mapped 2026-11-19 (training area + Gendarran Fields)
			// smooth route found by the Echo hunt in Hall of Chains (two characters, same route)
			{ 210545, 0x2124600, 0x108, 0x20, 0xC, { -1, -1, -1 }, { 0x10, 0x1A0, 0x48 }, 0x6D4 },
		};

		HANDLE    Self  = nullptr;
		uintptr_t Base  = 0;
		uintptr_t End   = 0;
	}

	const Layout* CurrentLayout(unsigned aBuild)
	{
		for (auto& l : LAYOUTS) { if (l.Build == aBuild) { return &l; } }
		return nullptr;
	}

	void Init()
	{
		Self = GetCurrentProcess();
		Base = (uintptr_t)GetModuleHandleW(nullptr);
		auto dos = (IMAGE_DOS_HEADER*)Base;
		auto nt  = (IMAGE_NT_HEADERS64*)(Base + dos->e_lfanew);
		End = Base + nt->OptionalHeader.SizeOfImage;
	}

	uintptr_t ExeBase() { return Base; }

	uintptr_t MovementObject(const Layout& aLayout, uintptr_t aAgent)
	{
		uint64_t p = aAgent;
		for (int hop : aLayout.SmoothHops)
		{
			uint64_t next = 0;
			if (!Read((uintptr_t)p + hop, next) || !LooksLikeHeapPtr(next)) { return 0; }
			p = next;
		}
		return (uintptr_t)p;
	}

	bool ReadSmoothPos(const Layout& aLayout, uintptr_t aAgent, Vec3& aOut)
	{
		const uintptr_t p = MovementObject(aLayout, aAgent);
		if (!p) { return false; }
		float f[3];
		if (!Read(p + aLayout.SmoothPosOffset, f, sizeof(f))) { return false; }
		aOut = GameToMumble(f);
		return std::isfinite(aOut.x) && std::isfinite(aOut.y) && std::isfinite(aOut.z);
	}
	bool InExe(uintptr_t p) { return p >= Base && p < End; }

	bool Read(uintptr_t aAddr, void* aOut, size_t aSize)
	{
		SIZE_T got = 0;
		return ReadProcessMemory(Self, (LPCVOID)aAddr, aOut, aSize, &got) && got == aSize;
	}

	bool LooksLikeHeapPtr(uint64_t q)
	{
		return q >= 0x10000 && q < 0x7FF000000000ull && (q & 7) == 0 && !InExe((uintptr_t)q);
	}

	std::vector<Region> ScannableRegions()
	{
		std::vector<Region> out;
		MEMORY_BASIC_INFORMATION mbi;
		uintptr_t addr = 0x10000;
		while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
		{
			bool rw  = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
			bool bad = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
			if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && !bad)
			{
				out.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize });
			}
			uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
			if (next <= addr) { break; }
			addr = next;
		}
		return out;
	}
}
