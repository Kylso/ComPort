#include "ComPort.h"
#include <iostream>

// The library "ComPort" for work with the serial port (RS-232).
// Only for windows (use windows.h function) and require c++14.
// Below is present the usage example of this module.
// Algorithm of example:
// 1. Initialize ComPort object.
// 2. Subscribe the callback function to rx data event.
// 3. Open connection.
// 4. Read data after receiving callback.
// 5. Write data.
// 6. If find the sequence of "END" [0x45, 0x4E, 0x44], then close connection, else go to 4.

// Tools to block the main thread, while waiting the callback function.
std::mutex mRxData;
std::condition_variable cvRxData;
bool isReleaseThread = false;

void rxDataCallback()
{
	std::unique_lock<std::mutex> lock{ mRxData };
	if (!isReleaseThread)
	{
		isReleaseThread = true;
		cvRxData.notify_one();
	}
}

enum class DetectEndState : uint8_t
{
	E = 0x45,
	N = 0x4E,
	D = 0x44
};

bool detectEnd(DetectEndState& detectEndState, uint8_t& data)
{
	bool result = false;
	switch (detectEndState)
	{
		case DetectEndState::E:
		{
			if (data == static_cast<uint8_t>(DetectEndState::E))
			{
				detectEndState = DetectEndState::N;
			}
			else
			{
				detectEndState = DetectEndState::E;
			}
			break;
		}
		case DetectEndState::N:
		{
			if (data == static_cast<uint8_t>(DetectEndState::N))
			{
				detectEndState = DetectEndState::D;
			}
			else
			{
				detectEndState = DetectEndState::E;
			}
			break;
		}
		case DetectEndState::D:
		{
			if (data == static_cast<uint8_t>(DetectEndState::D))
			{
				result = true;
			}
			else
			{
				detectEndState = DetectEndState::E;
			}
			break;
		}
	}
	return result;
}

int main()
{
	using ComPort = kylsocomport::ComPort;
	using Callback = kylsocomport::Callback;
	using UpCallback = kylsocomport::UpCallback;

	auto result = ComPort::Result::SUCCESS;	

	// Initialize ComPort object.
	ComPort comPort
	{
		2, // Number of comport.
		ComPort::Baudrate::_115200,
		ComPort::WordLength::_8,
		ComPort::StopBits::_1,
		ComPort::Parity::NO
	};

	// Subscribe the callback function to rx data event.
	UpCallback upCallback{ new Callback{ rxDataCallback } };
	comPort.setSubscribeOnEvent(ComPort::Event::RX_DATA, std::move(upCallback));
	
	// Open connection.
	result = comPort.open();
	if (result != ComPort::Result::SUCCESS)
	{
		return static_cast<int>(result);
	}

	std::unique_lock<std::mutex> lock{ mRxData, std::defer_lock };
	std::vector<uint8_t> data;
	std::string	answer;
	uint16_t rxDataCount;
	DetectEndState detectEndState = DetectEndState::E;
	bool isDetectEnd = false;

	while (true)
	{
		// Waiting the callback function.
		std::cout << "Wait data...\n";
		lock.lock();
		if (!isReleaseThread)
		{			
			cvRxData.wait(lock);			
		}
		isReleaseThread = false;
		lock.unlock();

		rxDataCount = comPort.getRxDataCount();
		if (rxDataCount > 0)
		{
			// Read data.
			std::cout << "Read data: ";
			comPort.rxData(data, rxDataCount);			
			for (auto i : data)
			{
				std::cout << std::hex << int(i) << ' ';
				if (!isDetectEnd)
				{
					isDetectEnd = detectEnd(detectEndState, i);
				}
			}
			if (isDetectEnd)
			{
				break;
			}

			// Write data.
			std::cout << "\n" << "Write data...\n";
			result = comPort.txData(data);
			if (result != ComPort::Result::SUCCESS)
			{
				break;
			}
			data.clear();
		}
	}

	// Close connection.
	comPort.close();

	return static_cast<int>(result);
}
