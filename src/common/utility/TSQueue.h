#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "stats.h"
#include "tarray.h"

template <typename T, int IN_NUM>
struct RingBuffer
{
	const int length = IN_NUM;
	T input[IN_NUM] = {};
	long pos = -1;

	void add(T value)
	{
		pos++;
		if (pos < 0) pos = 0;
		(*this)[0] = value;
	}

	T& operator[](int index)
	{
		assert(index >= 0 && index <= pos);
		return input[(pos - index) % IN_NUM];
	}

	void reset()
	{
		pos = -1;
	}
};

template <typename T>
class TSQueue
{
public:
	bool dequeue(T& item)
	{
		std::lock_guard<std::mutex> lock(mQueueLock);
		return mQueue.Pop(item);
	}

	void queue(const T& item)
	{
		std::lock_guard<std::mutex> lock(mQueueLock);
		mQueue.Insert(0, item);
	}

	void queue(T&& item)
	{
		std::lock_guard<std::mutex> lock(mQueueLock);
		mQueue.Insert(0, std::move(item));
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(mQueueLock);
		mQueue.Clear();
	}

	int deleteSearch(const std::function<bool(T&)>& func)
	{
		std::lock_guard<std::mutex> lock(mQueueLock);
		int originalSize = mQueue.Size();
		for (unsigned int i = 0; i < mQueue.Size(); i++)
		{
			if (func(mQueue[i]))
			{
				mQueue.Delete(i);
				i--;
			}
		}
		return originalSize - mQueue.Size();
	}

	bool dequeueSearch(T& item, const std::function<bool(T&)>& func)
	{
		std::lock_guard<std::mutex> lock(mQueueLock);
		for (unsigned int i = 0; i < mQueue.Size(); i++)
		{
			if (func(mQueue[i]))
			{
				item = std::move(mQueue[i]);
				mQueue.Delete(i);
				return true;
			}
		}
		return false;
	}

	int size() const
	{
		std::lock_guard<std::mutex> lock(mQueueLock);
		return mQueue.Size();
	}

private:
	mutable std::mutex mQueueLock;
	TArray<T> mQueue;
};

template <typename InputT, typename OutputT>
class ResourceLoader2
{
public:
	ResourceLoader2(TSQueue<InputT>* inputQueue, TSQueue<InputT>* secondaryInputQueue, TSQueue<OutputT>* outputQueue)
		: mInputQueue(inputQueue), mSecondaryInputQueue(secondaryInputQueue), mOutputQueue(outputQueue)
	{
	}

	virtual ~ResourceLoader2()
	{
		stop();
	}

	void start()
	{
		if (mThread.get_id() == std::thread::id())
		{
			mThread = std::thread(&ResourceLoader2::BackgroundProc, this);
		}
	}

	void stop()
	{
		if (mThread.joinable())
		{
			mActive.store(false);
			mWake.notify_all();
			mThread.join();
		}
	}

	void wake()
	{
		mWake.notify_all();
	}

	bool isActive() const
	{
		return mRunning.load();
	}

	int statTotalLoaded() const { return mStatTotalLoaded.load(); }
	double statAvgLoadTime() const { return mStatAvgTime.load(); }
	double statMinLoadTime() const { return std::min(99999.0, mStatMinTime.load()); }
	double statMaxLoadTime() const { return mStatMaxTime.load(); }

protected:
	virtual bool loadResource(InputT& input, OutputT& output) { return false; }
	virtual void prepareLoad() {}
	virtual void completeLoad() {}
	virtual void cancelLoad() {}

private:
	void BackgroundProc()
	{
		std::unique_lock<std::mutex> lock(mWakeLock);

		while (mActive.load())
		{
			bool processed = false;

			while (true)
			{
				if (mInputQueue->size() <= 0 && (mSecondaryInputQueue == nullptr || mSecondaryInputQueue->size() <= 0))
				{
					break;
				}

				mRunning.store(true);

				cycle_t loadTimer;
				loadTimer.Reset();
				loadTimer.Clock();

				prepareLoad();

				InputT input;
				if (!mInputQueue->dequeue(input))
				{
					if (mSecondaryInputQueue == nullptr || !mSecondaryInputQueue->dequeue(input))
					{
						cancelLoad();
						continue;
					}
				}

				OutputT output;
				if (loadResource(input, output))
				{
					mOutputQueue->queue(std::move(output));
				}
				processed = true;

				completeLoad();

				loadTimer.Unclock();
				mAccumulatedLoadTime += loadTimer.TimeMS();
				mAccumulatedLoadCount += 1;
				mStatAvgTime = mAccumulatedLoadTime / mAccumulatedLoadCount;
				mStatMinTime = std::min(mStatMinTime.load(), loadTimer.TimeMS());
				mStatMaxTime = std::max(mStatMaxTime.load(), loadTimer.TimeMS());
				mStatTotalLoaded++;
			}

			mRunning.store(false);

			if (!processed)
			{
				mWake.wait_for(lock, std::chrono::milliseconds(3));
			}
		}
	}

	std::atomic<bool> mActive{ true };
	std::atomic<bool> mRunning{ false };
	std::atomic<int> mStatTotalLoaded{ 0 };
	std::atomic<double> mStatAvgTime{ 0 };
	std::atomic<double> mStatMinTime{ 999999.0 };
	std::atomic<double> mStatMaxTime{ 0 };
	double mAccumulatedLoadTime = 0;
	double mAccumulatedLoadCount = 0;

	std::thread mThread;
	std::mutex mWakeLock;
	std::condition_variable mWake;
	TSQueue<InputT>* mInputQueue = nullptr;
	TSQueue<InputT>* mSecondaryInputQueue = nullptr;
	TSQueue<OutputT>* mOutputQueue = nullptr;
};
