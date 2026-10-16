#pragma once

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <Common/Log.h>
#include <Common/Time.h>

namespace D3dDdi
{
	// Counters for the verbose log summary at exit. They show which games spend time keeping the
	// system memory copies of video memory surfaces in sync (CPU locks and GetDC), and on which surfaces.
	namespace Stats
	{
		struct Transfer
		{
			unsigned long long count;
			unsigned long long bytes;
			long long qpcTime;
		};

		struct Entry
		{
			unsigned long long locks;
			unsigned long long readOnlyLocks;
			unsigned long long writeOnlyLocks;
			unsigned long long discardLocks;
			unsigned long long areaLocks;
			Transfer readbacks;
			Transfer uploads;
		};

		inline unsigned long long g_locks = 0;
		inline unsigned long long g_smallLocks = 0;
		inline unsigned long long g_smallLockBytes = 0;
		inline unsigned long long g_smallLockFullBytes = 0;
		inline unsigned long long g_surfaceDcs = 0;
		inline unsigned long long g_discardedReadbacks = 0;
		inline unsigned long long g_keyedWriteLocks = 0;
		inline Transfer g_readbacks = {};
		inline Transfer g_uploads = {};
		inline std::map<std::string, Entry> g_entries;

		inline Entry& getEntry(const std::string& key)
		{
			return g_entries[key];
		}

		inline double toMb(unsigned long long bytes)
		{
			return bytes / (1024.0 * 1024.0);
		}

		inline double toMs(long long qpc)
		{
			return 0 == Time::g_qpcFrequency ? 0.0 : qpc * 1000.0 / Time::g_qpcFrequency;
		}

		inline void log()
		{
			char text[400] = {};
			sprintf_s(text, "Surface sync: %llu CPU locks, %llu GetDC calls; %llu readbacks (%.1f MB, %.1f ms), "
				"%llu uploads (%.1f MB, %.1f ms); readbacks skipped: %llu discard locks, %llu write-only locks",
				g_locks, g_surfaceDcs,
				g_readbacks.count, toMb(g_readbacks.bytes), toMs(g_readbacks.qpcTime),
				g_uploads.count, toMb(g_uploads.bytes), toMs(g_uploads.qpcTime), g_discardedReadbacks, g_keyedWriteLocks);
			Compat::Log() << text;

			if (0 != g_smallLocks)
			{
				sprintf_s(text, "Surface sync: %llu locks of a small area caused full copies "
					"(%.1f MB copied, %.1f MB in the locked areas)",
					g_smallLocks, toMb(g_smallLockFullBytes), toMb(g_smallLockBytes));
				Compat::Log() << text;
			}

			std::vector<std::pair<std::string, Entry>> entries(g_entries.begin(), g_entries.end());
			std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
				return a.second.readbacks.qpcTime + a.second.uploads.qpcTime >
					b.second.readbacks.qpcTime + b.second.uploads.qpcTime; });
			for (std::size_t i = 0; i < entries.size() && i < 6; ++i)
			{
				const auto& e = entries[i].second;
				if (0 == e.readbacks.count && 0 == e.uploads.count)
				{
					break;
				}
				sprintf_s(text, "Surface sync: %s: %llu locks (%llu read-only, %llu write-only, %llu discard, "
					"%llu with area), %llu readbacks (%.1f MB, %.1f ms), %llu uploads (%.1f MB, %.1f ms)",
					entries[i].first.c_str(), e.locks, e.readOnlyLocks, e.writeOnlyLocks, e.discardLocks, e.areaLocks,
					e.readbacks.count, toMb(e.readbacks.bytes), toMs(e.readbacks.qpcTime),
					e.uploads.count, toMb(e.uploads.bytes), toMs(e.uploads.qpcTime));
				Compat::Log() << text;
			}
		}
	}
}
