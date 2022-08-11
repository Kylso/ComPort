#include "ComPort.h"
#include <string>
#include <thread>
#include <algorithm>
#include <chrono>

namespace kylsocomport
{

ComPort::ComPort(uint8_t portNum, Baudrate baudrate, WordLength wordLength,
				 StopBits stopBits, Parity parity) :
	portNum_(portNum), baudrate_(baudrate), wordLength_(wordLength),
	stopBits_(stopBits), parity_(parity)
{
	this->hComPort_ = nullptr;
    std::memset(&(this->dcbComPortParams_), 0, sizeof(this->dcbComPortParams_));
    std::memset(&(this->hRxOverlapped_), 0, sizeof(this->hRxOverlapped_));
	this->isOpen_ = false;
	this->rxQueueSize_ = 512;
	this->txDataQueueSize_ = 512;
	this->txDataQueueUse_ = 0;
	this->txOverlappedQueueSize_ = 5;
}

ComPort::~ComPort()
{
    this->close();
}

ComPort::Result ComPort::open()
{
	if (this->isOpen_)
	{
		return Result::ERROR_ALREADY_OPEN;
	}
	std::string portName = "\\\\.\\COM";
	if (this->portNum_ == 0)
	{
		return Result::ERROR_BAD_PORT_NUM;
	}
	portName += std::to_string(this->portNum_);
	this->hComPort_ = CreateFile(portName.c_str(), GENERIC_READ | GENERIC_WRITE,
								 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
	if (this->hComPort_ == INVALID_HANDLE_VALUE)
	{
		this->close();
		return Result::ERROR_OPEN;
	}
	std::memset(&(this->dcbComPortParams_), 0, sizeof(this->dcbComPortParams_));
	this->dcbComPortParams_.DCBlength = sizeof(this->dcbComPortParams_);
	if (!GetCommState(this->hComPort_, &(this->dcbComPortParams_)))
	{
		this->close();
		return Result::ERROR_SET_PORT_CONFIG;
	}
	this->dcbComPortParams_.BaudRate = static_cast<DWORD>(this->baudrate_);
	this->dcbComPortParams_.ByteSize = static_cast<BYTE>(this->wordLength_);
	this->dcbComPortParams_.StopBits = static_cast<BYTE>(this->stopBits_);
	this->dcbComPortParams_.Parity = static_cast<BYTE>(this->parity_);
	if (!SetCommState(this->hComPort_, &(this->dcbComPortParams_)))
	{
		this->close();
		return Result::ERROR_SET_PORT_CONFIG;
	}
    std::memset(&(this->hRxOverlapped_), 0, sizeof(this->hRxOverlapped_));
	this->hRxOverlapped_.hEvent = CreateEvent(nullptr, true, false, TEXT("UART RX DATA EVENT"));
	if (this->hRxOverlapped_.hEvent == nullptr)
	{
		this->close();
		return Result::ERROR_INIT_RX_EVENT;
	}
	this->isOpen_ = true;
    this->isReleaseTxDataThread_ = false;
    std::promise<void> rxThreadEndEventHandler;
    this->rxThreadEndEvent_ = rxThreadEndEventHandler.get_future();
    std::thread rxDataThread(&ComPort::doRxData, this, std::move(rxThreadEndEventHandler));
    rxDataThread.detach();
    std::promise<void> txThreadEndEventHandler;
    this->txThreadEndEvent_ = txThreadEndEventHandler.get_future();
    std::thread txDataThread(&ComPort::doTxData, this, std::move(txThreadEndEventHandler));
	txDataThread.detach();
	return Result::SUCCESS;
}

void ComPort::close()
{
	// Close port.
	this->isOpen_ = false;
	if (this->hComPort_ != nullptr) CloseHandle(this->hComPort_);
	if (this->hRxOverlapped_.hEvent != nullptr) CloseHandle(this->hRxOverlapped_.hEvent);
	this->hComPort_ = nullptr;
	this->hRxOverlapped_.hEvent = nullptr;

	// Clear queue.
    std::unique_lock<std::mutex> rxLock(this->rxQueueMutex_);
    std::unique_lock<std::mutex> txLock(this->txQueueMutex_);
	std::queue<uint8_t> emptyRxQueue;
	std::queue<TxQueueElement> emptyTxQueue;
	this->rxQueue_.swap(emptyRxQueue);
	this->txQueue_.swap(emptyTxQueue);
    rxLock.unlock();
    txLock.unlock();
    if (!this->isReleaseTxDataThread_)
    {
        std::unique_lock<std::mutex> threadWorkLock(this->txDataThreadMutex_);
        this->releaseTxDataThreadWork_.notify_one();
    }
    this->isReleaseTxDataThread_ = false;

    if (this->rxThreadEndEvent_.valid())
    {
        this->rxThreadEndEvent_.wait_for(std::chrono::seconds(1));
    }
    if (this->txThreadEndEvent_.valid())
    {
        this->txThreadEndEvent_.wait_for(std::chrono::seconds(1));
    }
}

uint16_t ComPort::getRxDataCount()
{
	std::lock_guard<std::mutex> lock(this->rxQueueMutex_);
	return static_cast<uint16_t>(this->rxQueue_.size());
}

void ComPort::rxData(std::vector<uint8_t>& data, uint16_t count)
{
	std::lock_guard<std::mutex> lock(this->rxQueueMutex_);
	while (!this->rxQueue_.empty() && count)
	{
		data.push_back(this->rxQueue_.front());
		this->rxQueue_.pop();
		count--;
	}
}

ComPort::Result ComPort::txData(std::vector<uint8_t> data)
{
    std::lock_guard<std::mutex> txQueueLock(this->txQueueMutex_);
    std::unique_lock<std::mutex> threadWorkLock(this->txDataThreadMutex_);

	if (!this->isOpen_)
	{
		return Result::ERROR_PORT_CLOSE;
    }

	// Check place for overlapped and data in fifo.
	bool hasPlaceForData = static_cast<uint16_t>(data.size()) <
		(this->txDataQueueSize_ - this->txDataQueueUse_);

    if ((this->txQueue_.size() == this->txOverlappedQueueSize_) ||
		(!hasPlaceForData))
	{
		return Result::ERROR_TX_QUEUE_FULL;
	}

	// Create new overlapped object.
	OVERLAPPED* txOverlapped = new OVERLAPPED;
	std::memset(txOverlapped, 0, sizeof(*txOverlapped));
	txOverlapped->hEvent = CreateEvent(nullptr, true, false, nullptr);
	if (txOverlapped->hEvent == nullptr)
	{
		delete txOverlapped;
		return Result::ERROR_INIT_TX_EVENT;
	}

	// Add data to tx queue.
    this->txDataQueueUse_ += static_cast<uint16_t>(data.size());
    auto upTxOverlapped = std::unique_ptr<OVERLAPPED>{txOverlapped};
    TxQueueElement txQueueElement{std::move(upTxOverlapped), std::move(data)};
    this->txQueue_.push(std::move(txQueueElement));

	// Disable thread block if need.
    if (!this->isReleaseTxDataThread_)
	{
        this->releaseTxDataThreadWork_.notify_one();
	}
    this->isReleaseTxDataThread_ = true;

	return Result::SUCCESS;
}

std::string ComPort::getTextOfResult(Result result) const
{
    std::string sResult;
    switch (result)
    {
        case Result::SUCCESS:
            sResult = "success";
            break;
        case Result::ERROR_ALREADY_OPEN:
            sResult = "comport already open";
            break;
        case Result::ERROR_BAD_PORT_NUM:
            sResult = "bad comport num";
            break;
        case Result::ERROR_OPEN:
            sResult = "cant open";
            break;
        case Result::ERROR_SET_PORT_CONFIG:
            sResult = "cant set comport config";
            break;
        case Result::ERROR_INIT_RX_EVENT:
            sResult = "cant initialize rx event";
            break;
        case Result::ERROR_PORT_CLOSE:
            sResult = "comport is close";
            break;
        case Result::ERROR_TX_QUEUE_FULL:
            sResult = "tx queue full";
            break;
        case Result::ERROR_INIT_TX_EVENT:
            sResult = "cant initialize tx event";
            break;
    }
    return sResult;
}

void ComPort::setSubscribeOnEvent(Event event, UpCallback callback)
{
    std::vector<UpCallback>* callbacks;
	if (event == Event::RX_DATA)
	{
		callbacks = &this->rxDataCallbacks_;
	}
	else // event == Event::SHUTDOWN
	{
		callbacks = &this->shutdownCallbacks_;
	}

    this->callbackMutex_.lock();
    for (auto& i : *callbacks)
    {
        if (i.get() == callback.get())
        {
            this->callbackMutex_.unlock();
            return; // This callback already set.
        }
    }
    // This callback not set.
    callbacks->push_back(std::move(callback));
    this->callbackMutex_.unlock();
}

void ComPort::resetSubscribeOnEvent(Event event, UpCallback callback)
{
    std::vector<UpCallback>* callbacks;
	if (event == Event::RX_DATA)
	{
		callbacks = &this->rxDataCallbacks_;
	}
	else // event == Event::SHUTDOWN
	{
		callbacks = &this->shutdownCallbacks_;
    }

    this->callbackMutex_.lock();
    for (auto i = callbacks->begin(); i != callbacks->end(); ++i)
    {
        if (i->get() == callback.get())
        {
            callbacks->erase(i); // This callback set.
            break;
        }
    }
    this->callbackMutex_.unlock();
}

void ComPort::doRxData(std::promise<void> endEventHadler)
{
	uint8_t byte;
	DWORD rxDataCnt;
	std::unique_lock<std::mutex> rxQueueLock(this->rxQueueMutex_, std::defer_lock);
	bool isShutdown = false, isRxByte = false;
	while (this->isOpen_)
	{
		if (!ReadFile(this->hComPort_, &byte, 1, &rxDataCnt, &this->hRxOverlapped_))
		{
			if (GetLastError() != ERROR_IO_PENDING)
			{
				isShutdown = true;
                break;
			}
			if (WaitForSingleObject(this->hRxOverlapped_.hEvent, INFINITE) == WAIT_OBJECT_0)
			{
				if (!GetOverlappedResult(this->hComPort_, &this->hRxOverlapped_,
                                         &rxDataCnt, false))
				{
					isShutdown = true;
					break;
				}
				isRxByte = true;
			}
			else
			{
				isShutdown = true;
                break;
			}
		}
		else
		{
			isRxByte = true;			
		}
		if (isRxByte)
		{
			isRxByte = false;
			rxQueueLock.lock();
			if (this->rxQueue_.size() != this->rxQueueSize_)
			{
				this->rxQueue_.push(byte);
            }
            rxQueueLock.unlock();
            this->callbackMutex_.lock();
            for (auto& callback : this->rxDataCallbacks_)
			{
                (*(callback.get()))();
			}
            this->callbackMutex_.unlock();
		}
	}
	if (isShutdown)
	{
        this->callbackMutex_.lock();
        for (auto& callback : this->shutdownCallbacks_)
        {
            (*(callback.get()))();
        }
        this->callbackMutex_.unlock();
	}
    endEventHadler.set_value();
}

void ComPort::doTxData(std::promise<void> endEventHadler)
{
    std::unique_lock<std::mutex>    txQueueLock(this->txQueueMutex_,
                                                std::defer_lock);

    std::unique_lock<std::mutex>    threadWorkLock(this->txDataThreadMutex_,
                                                   std::defer_lock);

    bool                            isShutdown = false;
    std::vector<uint8_t>            data;
    DWORD                           txDataCnt;
    std::unique_ptr<OVERLAPPED>     txOverlapped;

	while (this->isOpen_)
    {
        threadWorkLock.lock();
        if (!this->isReleaseTxDataThread_)
        {
            this->releaseTxDataThreadWork_.wait(threadWorkLock);            
        }
        this->isReleaseTxDataThread_ = false;
        threadWorkLock.unlock();

        txQueueLock.lock();
        if (this->txQueue_.size() > 0)
        {
            txOverlapped = std::move(this->txQueue_.front().first);
            data = std::move(this->txQueue_.front().second);
            this->txDataQueueUse_ -= static_cast<uint16_t>(data.size());
            this->txQueue_.pop();
            txQueueLock.unlock();
        }
        else
        {
            txQueueLock.unlock();
            continue;
        }

        if (!WriteFile(this->hComPort_, data.data(), static_cast<DWORD>(data.size()),
                       &txDataCnt, txOverlapped.get()))
        {
            if (GetLastError() != ERROR_IO_PENDING)
            {
                isShutdown = true;
                break;
            }
            bool isTxSuccessful = false;
            auto waitResult = WaitForSingleObject(txOverlapped->hEvent,
                                                  INFINITE);
            if (waitResult == WAIT_OBJECT_0)
            {
                if (GetOverlappedResult(this->hComPort_, txOverlapped.get(),
                                        &txDataCnt, false))
                {
                    if (txDataCnt == data.size())
                    {
                        isTxSuccessful = true;
                    }
                }
            }
            if (!isTxSuccessful)
            {
                isShutdown = true;
                break;
            }
        }
        if (txDataCnt != data.size())
        {
            isShutdown = true;
            break;
        }

		if (isShutdown)
		{
            this->callbackMutex_.lock();
            for (auto& callback : this->shutdownCallbacks_)
            {
                (*(callback.get()))();
            }
            this->callbackMutex_.unlock();
			break;
		}
	}
    endEventHadler.set_value();
}

}
