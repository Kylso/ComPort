# General information

**ComPort.cpp / ComPort.h** - The library for work with the serial port (RS-232).
It use the WinAPI function (windows.h), therefore it only compatible with Windows.
Processes of read/write data do in other threads.
**Main.cpp** contain a basic example of working with the library.

Algorithm of example:
1. Initialize ComPort object.
2. Subscribe the callback function to rx data event.
3. Open connection.
4. Read data after receiving callback.
5. Write data.
6. If find the sequence of "END" [0x45, 0x4E, 0x44], then close connection, else go to 4.

# Requirements

Minimum C++14. OS Windows.