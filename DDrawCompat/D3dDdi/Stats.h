#pragma once

#include <cstdio>

#include <Common/Log.h>
#include <Common/Time.h>

namespace D3dDdi
{
	// Counters for the verbose log summary at exit. They show which games spend time keeping the
	// system memory copies of video memory surfaces in sync (CPU locks and GetDC).
	namespace Stats
	{
		struct Transfer
		{
			unsigned long long count;
			unsigned long long bytes;
			long long qpcTime;
		};

		inline unsigned long long g_locks = 0;
		inline unsigned long long g_smallLocks = 0;
		inline unsigned long long g_smallLockBytes = 0;
		inline unsigned long long g_smallLockFullBytes = 0;
		inline unsigned long long g_surfaceDcs = 0;
		inline Transfer g_readbacks = {};
		inline Transfer g_uploads = {};

		inline void log()
		{
			auto mb = [](unsigned long long bytes) { return bytes / (1024.0 * 1024.0); };
			auto ms = [](long long qpc) { return 0 == Time::g_qpcFrequency ? 0.0 : qpc * 1000.0 / Time::g_qpcFrequency; };

			char text[300] = {};
			sprintf_s(text, "Surface sync: %llu CPU locks, %llu GetDC calls; %llu readbacks (%.1f MB, %.1f ms), "
				"%llu uploads (%.1f MB, %.1f ms)",
				g_locks, g_surfaceDcs,
				g_readbacks.count, mb(g_readbacks.bytes), ms(g_readbacks.qpcTime),
				g_uploads.count, mb(g_uploads.bytes), ms(g_uploads.qpcTime));
			Compat::Log() << text;

			if (0 != g_smallLocks)
			{
				sprintf_s(text, "Surface sync: %llu locks of a small area caused full copies "
					"(%.1f MB copied, %.1f MB in the locked areas)",
					g_smallLocks, mb(g_smallLockFullBytes), mb(g_smallLockBytes));
				Compat::Log() << text;
			}
		}
	}
}
