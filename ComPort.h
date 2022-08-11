#pragma once

#include <windows.h>
#include <queue>
#include <mutex>
#include <cstdint>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <string>
#include <future>
#include <functional>

namespace kylsocomport
{

using TxQueueElement = std::pair<std::unique_ptr<OVERLAPPED>, std::vector<uint8_t>>;
using Callback = std::function<void(void)>;
using UpCallback = std::unique_ptr<Callback>;

class ComPort final
{
public:
	enum class Result
	{
		SUCCESS,
		ERROR_ALREADY_OPEN,
		ERROR_BAD_PORT_NUM,
		ERROR_OPEN,
		ERROR_SET_PORT_CONFIG,
		ERROR_INIT_RX_EVENT,
		ERROR_PORT_CLOSE,
		ERROR_TX_QUEUE_FULL,
		ERROR_INIT_TX_EVENT
	};

	enum class Baudrate
	{
		_110	= CBR_110,
		_300	= CBR_300,
		_600	= CBR_600,
		_1200	= CBR_1200,
		_2400	= CBR_2400,
		_4800	= CBR_4800,
		_9600	= CBR_9600,
		_14400	= CBR_14400,
		_19200	= CBR_19200,
		_38400	= CBR_38400,
		_56000	= CBR_56000,
		_57600	= CBR_57600,
		_115200 = CBR_115200,
		_128000 = CBR_128000,
		_256000 = CBR_256000
	};

	enum class WordLength
	{
		_7 = 7,
		_8,
		_9
	};

	enum class StopBits
	{
		_1,
		_1_5,
		_2
	};

	enum class Parity
	{
		NO,
		ODD,
		EVEN
	};

	enum class Event
	{
		RX_DATA,
		SHUTDOWN
	};

	ComPort(uint8_t portNum, Baudrate baudrate, WordLength wordLength,
			StopBits stopBits, Parity parity);

	~ComPort();

	Result open();

	void close();

	bool isOpen() const
	{
		return this->isOpen_;
	};

	bool setPortNum(uint8_t portNum)
	{
		if (this->isOpen_ || portNum == 0)
		{
			return false;
		}
		else
		{
			this->portNum_ = portNum;
			return true;
		}
	}

	uint8_t getPortNum() const
	{
		return this->portNum_;
	}

	bool setBaudrate(Baudrate baudrate)
	{
		if (this->isOpen_)
		{
			return false;
		}
		else
		{
			this->baudrate_ = baudrate;
			return true;
		}
	}

	Baudrate getBaudrate() const
	{
		return this->baudrate_;
	}

	bool setWordLength(WordLength wordLength)
	{
		if (this->isOpen_)
		{
			return false;
		}
		else
		{
			this->wordLength_ = wordLength;
			return true;
		}
	}

	WordLength getWordLength() const
	{
		return this->wordLength_;
	}

	bool setStopBits(StopBits stopBits)
	{
		if (this->isOpen_)
		{
			return false;
		}
		else
		{
			this->stopBits_ = stopBits;
			return true;
		}
	}

	StopBits getStopBits() const
	{
		return this->stopBits_;
	}

	bool setParity(Parity parity)
	{
		if (this->isOpen_)
		{
			return false;
		}
		else
		{
			this->parity_ = parity;
			return true;
		}
	}

	Parity getParity() const
	{
		return this->parity_;
	}

	// Return count of data in rx fifo.
	uint16_t getRxDataCount();

	// Get data from rx fifo into vector.
	// Count - count of data then will be read from rx fifo.
	// If count greater count of data in rx fifo then read all rx fifo.
	void rxData(std::vector<uint8_t>& data, uint16_t count);	

	Result txData(std::vector<uint8_t> data);

    std::string getTextOfResult(Result result) const;

	// Set subscribe on event.
    void setSubscribeOnEvent(Event event, UpCallback callback);

	// Reset subscribe on event.
    void resetSubscribeOnEvent(Event event, UpCallback callback);

private:
	HANDLE						hComPort_; // Comport object.
	DCB							dcbComPortParams_; // Comport settings object.
	OVERLAPPED					hRxOverlapped_; // Async rx data object.
	std::atomic<bool>			isOpen_;

	// Comport settings.
	uint8_t						portNum_;
	Baudrate					baudrate_;
	WordLength					wordLength_;
	StopBits					stopBits_;
	Parity						parity_;

	// Fields for rx queue.
	uint16_t					rxQueueSize_;
	std::queue<uint8_t>			rxQueue_;
	std::mutex					rxQueueMutex_;

	// Fields for tx queue.
	uint16_t					txDataQueueSize_;
	std::atomic<uint16_t>		txDataQueueUse_;
	uint8_t						txOverlappedQueueSize_;
	std::queue<TxQueueElement>	txQueue_;
	std::mutex					txQueueMutex_;

    std::condition_variable		releaseTxDataThreadWork_;
	std::mutex					txDataThreadMutex_;
    bool						isReleaseTxDataThread_;

    // Fields for wait of thread end.
    std::future<void>           rxThreadEndEvent_;
    std::future<void>           txThreadEndEvent_;

	// Fields for handle callback.
    std::vector<UpCallback>		rxDataCallbacks_;
    std::vector<UpCallback>		shutdownCallbacks_;
    std::mutex                  callbackMutex_;

	// Method for rx data in other thread.
    void doRxData(std::promise<void> endEventHadler);

	// Method for tx data in other thread.
    void doTxData(std::promise<void> endEventHadler);
};

} // usercomport
