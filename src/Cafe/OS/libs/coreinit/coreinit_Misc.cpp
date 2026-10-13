#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Misc.h"
#include "Cafe/OS/libs/coreinit/coreinit_MessageQueue.h"
#include "Cafe/OS/libs/coreinit/coreinit_OSScreen.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Filesystem/fsc.h"
#include "config/LaunchSettings.h"
#include <pugixml.hpp>
#include "DualscreenSession.h"

namespace coreinit
{
	sint32 ppc_vcprintf_pad(char* strOut, sint32 maxLength, sint32 padLength, char padChar)
	{
		padLength = std::min<sint32>(padLength, maxLength);
		sint32 paddedLength = padLength;
		while ( padLength > 0)
		{
			*strOut = padChar;
			strOut++;
			padLength--;
		}
		return paddedLength;
	}

	// handler for %s
	sint32 ppc_vcprintf_str(char* strOut, sint32 maxLength, MEMPTR<char> strPointer, std::optional<sint32> width, std::optional<sint32> precision)
	{
		if (maxLength <= 0)
			return 0;
		const char* stringData = "(null)";
		if (strPointer)
			stringData = strPointer.GetPtr();
		else
		{
			if (precision.has_value() && *precision > 6)
				precision = 6;
		}
		// get string length
		sint32 stringLength = 0;
		if (precision.has_value())
		{
			stringLength = *precision;
			for (sint32 i=0; i<stringLength; i++)
			{
				if (stringData[i] == '\0')
				{
					stringLength = i;
					break;
				}
			}
		}
		else
			stringLength = strlen(stringData);
		if (stringLength < 0)
		{
			cemuLog_log(LogType::APIErrors, "ppc_vprintf: String length less than zero");
			stringLength = 0;
		}
		sint32 outputLength = 0;
		if (width.has_value() && width.value() > 0)
		{
			// left padding
			sint32 paddedLength = *width;
			paddedLength -= stringLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}
		stringLength = std::min<sint32>(stringLength, maxLength);
		cemu_assert(stringLength >= 0);
		memcpy(strOut, stringData, stringLength);
		outputLength += stringLength;
		if (width.has_value() && width.value() < 0)
		{
			sint32 paddedLength = -*width;
			paddedLength -= stringLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength - stringLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut + stringLength, maxLength - stringLength, paddedLength, ' ');
				outputLength += paddedLength;
			}
		}
		return outputLength;
	}

	// handler for %c
	sint32 ppc_vcprintf_char(char* strOut, sint32 maxLength, char character, std::optional<sint32> width, std::optional<sint32> precision)
	{
		if (maxLength <= 0)
			return 0;
		sint32 outputLength = 0;
		if (width.has_value() && width.value() > 0)
		{
			// left padding
			sint32 paddedLength = *width;
			paddedLength -= 1;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}
		if (maxLength > 0)
		{
			*strOut = character;
			outputLength++;
		}
		if (width.has_value() && width.value() < 0)
		{
			sint32 paddedLength = -*width;
			paddedLength -= 1;
			paddedLength = std::min<sint32>(paddedLength, maxLength - 1);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut + 1, maxLength - 1, paddedLength, ' ');
				outputLength += paddedLength;
			}
		}
		return outputLength;
	}

	char GetHexDigit(uint8 nibble, bool isUppercase)
	{
		if (nibble >= 10)
		{
			return isUppercase ? (nibble - 10 + 'A') : (nibble - 10 + 'a');
		}
		return nibble + '0';
	}

	// handler for integers
	sint32 ppc_vcprintf_integer(char* strOut, sint32 maxLength, bool isNegative, uint64 absValue, bool isHex, bool uppercase, bool alternateForm, std::optional<sint32> width, std::optional<sint32> precision, char padChar)
	{
		if (maxLength <= 0)
			return 0;

		// convert to digits string
		char digitBuf[64];
		sint32 digitCount = 0;
		uint64 tmpValue = absValue;
		if (isHex)
		{
			do
			{
				digitBuf[digitCount++] = GetHexDigit(tmpValue&0xF, uppercase);
				tmpValue >>= 4;
			}
			while (tmpValue != 0);
		}
		else
		{
			do
			{
				digitBuf[digitCount++] = (char)('0' + (tmpValue % 10));
				tmpValue /= 10;
			}
			while (tmpValue != 0);
		}

		// handle "%.0d"
		if (precision.has_value() && *precision == 0 && (!isNegative && absValue == 0))
			digitCount = 0;

		// precision adds leading zeroes for digits only
		sint32 precisionZeroes = 0;
		if (precision.has_value() && *precision > digitCount)
			precisionZeroes = *precision - digitCount;

		sint32 numberLength = digitCount + precisionZeroes;
		if (isNegative)
			numberLength++; // count the sign
		if (isHex && alternateForm && maxLength >= 2 && absValue != 0)
			numberLength += 2;

		sint32 outputLength = 0;

		// zero padding is a special case because its between the sign and the number
		bool widthZeroPad = width.has_value() && *width > 0 && padChar == '0' && !precision.has_value();

		// handle space padding left
		if (width.has_value() && *width > 0 && !widthZeroPad)
		{
			sint32 paddedLength = *width - numberLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}

		// sign
		if (isNegative && maxLength > 0)
		{
			*strOut = '-';
			strOut++;
			maxLength--;
			outputLength++;
		}
		// hex alternate form prefix (0x / 0X)
		bool hasHexPrefix = false;
		if (isHex && alternateForm && maxLength >= 2 && absValue != 0)
		{
			strOut[0] = '0';
			if (uppercase)
				strOut[1] = 'X';
			else
				strOut[1] = 'x';
			strOut += 2;
			maxLength -= 2;
			outputLength += 2;
			hasHexPrefix = true;
		}
		// output zero padding after sign
		if (widthZeroPad)
		{
			sint32 paddedLength = *width - (digitCount + (isNegative ? 1 : 0));
			if (hasHexPrefix)
				paddedLength -= 2;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, '0');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}
		else if (precisionZeroes > 0)
		{
			// precision zeroes
			sint32 paddedLength = std::min<sint32>(precisionZeroes, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, '0');
				strOut += paddedLength;
				maxLength -= paddedLength;
				outputLength += paddedLength;
			}
		}

		// digits (the buffer is in reverse order)
		sint32 writableDigits = std::min<sint32>(digitCount, maxLength);
		for (sint32 i = 0; i < writableDigits; i++)
			strOut[i] = digitBuf[digitCount - 1 - i];
		strOut += writableDigits;
		maxLength -= writableDigits;
		outputLength += writableDigits;

		// space padding right
		if (width.has_value() && *width < 0)
		{
			sint32 paddedLength = (-*width) - numberLength;
			paddedLength = std::min<sint32>(paddedLength, maxLength);
			if (paddedLength > 0)
			{
				ppc_vcprintf_pad(strOut, maxLength, paddedLength, ' ');
				outputLength += paddedLength;
			}
		}
		return outputLength;
	}

	// handler for signed decimal integers (64-bit core)
	sint32 ppc_vcprintf_decimal64(char* strOut, sint32 maxLength, bool isNegative, uint64 absValue, std::optional<sint32> width, std::optional<sint32> precision, char padChar)
	{
		return ppc_vcprintf_integer(strOut, maxLength, isNegative, absValue, false, false, false, width, precision, padChar);
	}

	sint32 ppc_vprintf(const char* formatStr, char* strOut, sint32 maxLength, ppc_va_list* vargs)
	{
		if (maxLength <= 0)
			return 0;
		char tempStr[4096];
		sint32 writeIndex = 0;
		while (*formatStr)
		{
			char c = *formatStr;
			if (c == '%')
			{
				const char* formatStart = formatStr;
				formatStr++;
				if (*formatStr == '%')
				{
					// percent sign
					if (writeIndex >= maxLength)
						break;
					strOut[writeIndex] = '%';
					writeIndex++;
					formatStr++;
					continue;
				}
				// flags
				char padChar = ' ';
				bool alternateForm = false;
				bool flag_leftJustify = false;
				if (*formatStr == '-')
				{
					flag_leftJustify = true;
					formatStr++;
				}
				if (*formatStr == '+')
				{
					formatStr++;
				}
				if (*formatStr == ' ')
				{
					formatStr++;
				}
				if (*formatStr == '#')
				{
					alternateForm = true;
					formatStr++;
				}
				if (*formatStr == '0')
				{
					padChar = '0';
					formatStr++;
				}
				// width
				std::optional<sint32> width;
				if (*formatStr == '*')
				{
					formatStr++;
					width = *(sint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					if (flag_leftJustify)
						width = -abs(*width);
				}
				else
				{
					width = 0;
					while (*formatStr >= '0' && *formatStr <= '9')
					{
						*width *= 10;
						*width += (*formatStr - '0');
						formatStr++;
					}
					if (flag_leftJustify)
						width = -*width;
				}
				// precision
				std::optional<sint32> precision;
				if (*formatStr == '.')
				{
					formatStr++;
					if (*formatStr == '*')
					{
						precision = *(sint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
						if (*precision < 0)
							precision.reset();
						formatStr++;
					}
					else
					{
						precision = 0;
						while (*formatStr >= '0' && *formatStr <= '9')
						{
							*precision *= 10;
							*precision += (*formatStr - '0');
							formatStr++;
						}
					}
				}
				// length + specifier
				char tempFormat[64];
				if (strncmp(formatStr, "lld", 3) == 0 || strncmp(formatStr, "lli", 3) == 0)
				{
					// 64bit decimal (signed)
					formatStr += 3;
					sint64 v = *(sint64be*)_ppc_va_arg(vargs, ppc_va_type::INT64);
					if (v >= 0)
						writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, width, precision, padChar);
					else
						writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, true, ~(uint64)v + 1, width, precision, padChar);
				}
				else if (strncmp(formatStr, "llu", 3) == 0)
				{
					// 64bit decimal (unsigned)
					formatStr += 3;
					uint64 v = *(uint64be*)_ppc_va_arg(vargs, ppc_va_type::INT64);
					writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, false, v, width, precision, padChar);
				}
				else if (*formatStr == 'd' || *formatStr == 'i' || strncmp(formatStr, "ld", 2) == 0 || strncmp(formatStr, "li", 2) == 0)
				{
					// 32bit decimal (signed)
					if (formatStr[0] == 'l')
						formatStr += 2;
					else
						formatStr++;
					sint32 v = *(sint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, v < 0, (uint64)abs((sint64)v), width, precision, padChar);
				}
				else if (*formatStr == 'u' || strncmp(formatStr, "lu", 2) == 0)
				{
					// 32bit decimal (unsigned)
					if (formatStr[0] == 'l')
						formatStr += 2;
					else
						formatStr++;
					uint32 v = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_decimal64(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, width, precision, padChar);
				}
				else if (strncmp(formatStr, "llx", 3) == 0 || strncmp(formatStr, "llX", 3) == 0)
				{
					// 64bit hexadecimal
					bool isUppercase = formatStr[2] == 'X';
					formatStr += 3;
					uint64 v = *(uint64be*)_ppc_va_arg(vargs, ppc_va_type::INT64);
					writeIndex += ppc_vcprintf_integer(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, true, isUppercase, alternateForm, width, precision, padChar);
				}
				else if (*formatStr == 'x' || *formatStr == 'X')
				{
					// 32bit hexadecimal
					bool isUppercase = *formatStr == 'X';
					formatStr++;
					uint32 v = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_integer(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, true, isUppercase, alternateForm, width, precision, padChar);
				}
				else if (*formatStr == 'p')
				{
					// pointer
					formatStr++;
					uint32 v = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					if (!precision.has_value())
						precision = 8;
					writeIndex += ppc_vcprintf_integer(strOut + writeIndex, maxLength - writeIndex, false, (uint64)v, true, false, true, width, precision, padChar);
				}
				else if (*formatStr == 's')
				{
					// string
					formatStr++;
					MPTR strOffset = *(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_str(strOut + writeIndex, maxLength - writeIndex, MEMPTR<char>(strOffset), width, precision);
				}
				else if (*formatStr == 'c')
				{
					// character
					formatStr++;
					char character = (char)(uint32)*(uint32be*)_ppc_va_arg(vargs, ppc_va_type::INT32);
					writeIndex += ppc_vcprintf_char(strOut + writeIndex, maxLength - writeIndex, character, width, precision);
				}
				else if (*formatStr == 'f' || *formatStr == 'g' || *formatStr == 'G')
				{
					formatStr++;
					strncpy(tempFormat, formatStart, std::min((std::ptrdiff_t)sizeof(tempFormat) - 1, formatStr - formatStart));
					if ((formatStr - formatStart) < sizeof(tempFormat))
						tempFormat[(formatStr - formatStart)] = '\0';
					else
						tempFormat[sizeof(tempFormat) - 1] = '\0';
					sint32 tempLen = sprintf(tempStr, tempFormat, (double)*(betype<double>*)_ppc_va_arg(vargs, ppc_va_type::FLOAT_OR_DOUBLE));
					for (sint32 i = 0; i < tempLen; i++)
					{
						if (writeIndex >= maxLength)
							break;
						strOut[writeIndex] = tempStr[i];
						writeIndex++;
					}
				}
				else if (formatStr[0] == 'l' && formatStr[1] == 'f')
				{
					// double
					formatStr += 2;
					strncpy(tempFormat, formatStart, std::min((std::ptrdiff_t)sizeof(tempFormat) - 1, formatStr - formatStart));
					if ((formatStr - formatStart) < sizeof(tempFormat))
						tempFormat[(formatStr - formatStart)] = '\0';
					else
						tempFormat[sizeof(tempFormat) - 1] = '\0';
					sint32 tempLen = sprintf(tempStr, tempFormat, (double)*(betype<double>*)_ppc_va_arg(vargs, ppc_va_type::FLOAT_OR_DOUBLE));
					for (sint32 i = 0; i < tempLen; i++)
					{
						if (writeIndex >= maxLength)
							break;
						strOut[writeIndex] = tempStr[i];
						writeIndex++;
					}
				}
				else
				{
					// unsupported / unknown specifier
					cemu_assert_debug(false);
					break;
				}
				cemu_assert(writeIndex <= maxLength); // sanity check
			}
			else
			{
				if (writeIndex >= maxLength)
					break;
				strOut[writeIndex] = c;
				writeIndex++;
				formatStr++;
			}
		}
		writeIndex = std::min<sint32>(writeIndex, maxLength-1);
		strOut[writeIndex] = '\0';
		return writeIndex;
	}

	/* coreinit logging and string format */

	sint32 __os_snprintf(char* outputStr, sint32 maxLength, const char* formatStr)
	{
		ppc_define_va_list(3, 0);
		sint32 r = ppc_vprintf(formatStr, outputStr, maxLength, &vargs);
		return r;
	}

	enum class CafeLogType
	{
		OSCONSOLE = 0,
	};

	struct CafeLogBuffer
	{
		std::array<char, 270> lineBuffer;
		size_t lineLength{};
	};

	CafeLogBuffer g_logBuffer_OSReport;

	CafeLogBuffer& getLogBuffer(CafeLogType cafeLogType)
	{
		if (cafeLogType == CafeLogType::OSCONSOLE)
			return g_logBuffer_OSReport;
		// default to OSReport
		return g_logBuffer_OSReport;
	}

	std::string_view getLogBufferName(CafeLogType cafeLogType)
	{
		if (cafeLogType == CafeLogType::OSCONSOLE)
			return "OSConsole";
		return "Unknown";
	}

	std::mutex sCafeConsoleMutex;
	bool s_forwardConsoleLogs;
	
	void WriteCafeConsole(CafeLogType cafeLogType, const char* msg, sint32 len)
	{
		std::unique_lock _l(sCafeConsoleMutex);
		CafeLogBuffer& logBuffer = getLogBuffer(cafeLogType);
		// once a line is full or \n is written it will be posted to log
		auto flushLine = [](CafeLogBuffer& cafeLogBuffer, std::string_view cafeLogName) -> void
		{
			cemuLog_log(LogType::CoreinitLogging, "[{0}] {1}", cafeLogName, std::basic_string_view(cafeLogBuffer.lineBuffer.data(), cafeLogBuffer.lineLength));
			cafeLogBuffer.lineLength = 0;
		};

		if (s_forwardConsoleLogs) {
			if (cafeLogType == CafeLogType::OSCONSOLE) {
				fwrite(msg, 1, len, stdout);
				fflush(stdout);
			} else {
				fwrite(msg, 1, len, stderr);
			}
		}
		while (len)
		{
			char c = *msg;
			msg++;
			len--;
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				// flush line immediately
				flushLine(logBuffer, getLogBufferName(cafeLogType));
				continue;
			}
			logBuffer.lineBuffer[logBuffer.lineLength] = c;
			logBuffer.lineLength++;
			if (logBuffer.lineLength >= logBuffer.lineBuffer.size())
				flushLine(logBuffer, getLogBufferName(cafeLogType));
		}
	}

	void COSVReport(COSReportModule module, COSReportLevel level, const char* format, ppc_va_list* vargs)
	{
		char tmpBuffer[1024];
		sint32 len = ppc_vprintf(format, tmpBuffer, sizeof(tmpBuffer), vargs);
		WriteCafeConsole(CafeLogType::OSCONSOLE, tmpBuffer, len);
	}

	void OSReport(const char* format)
	{
		ppc_define_va_list(1, 0);
		COSVReport(COSReportModule::coreinit, COSReportLevel::Info, format, &vargs);
	}

	void OSVReport(const char* format, ppc_va_list* vargs)
	{
		COSVReport(COSReportModule::coreinit, COSReportLevel::Info, format, vargs);
	}

	void COSWarn(int moduleId, const char* format)
	{
		ppc_define_va_list(2, 0);
		char tmpBuffer[1024];
		int prefixLen = sprintf(tmpBuffer, "[COSWarn-%d] ", moduleId);
		sint32 len = ppc_vprintf(format, tmpBuffer + prefixLen, sizeof(tmpBuffer) - prefixLen, &vargs);
		WriteCafeConsole(CafeLogType::OSCONSOLE, tmpBuffer, len + prefixLen);
	}

	void OSLogPrintf(int ukn1, int ukn2, int ukn3, const char* format)
	{
		ppc_define_va_list(4, 0);
		char tmpBuffer[1024];
		int prefixLen = sprintf(tmpBuffer, "[OSLogPrintf-%d-%d-%d] ", ukn1, ukn2, ukn3);
		sint32 len = ppc_vprintf(format, tmpBuffer + prefixLen, sizeof(tmpBuffer) - prefixLen, &vargs);
		WriteCafeConsole(CafeLogType::OSCONSOLE, tmpBuffer, len + prefixLen);
	}

	void OSConsoleWrite(const char* strPtr, sint32 length)
	{
		if (length < 0)
			return;
		WriteCafeConsole(CafeLogType::OSCONSOLE, strPtr, length);
	}

	void OSFatal(const char* msg)
	{
		// print to log
		ppc_define_va_list(1, 0);
		COSVReport(COSReportModule::coreinit, COSReportLevel::Error, msg, &vargs);

		// print to screen
		OSScreenInit();
		uint32 tvBufferSize = OSScreenGetBufferSizeEx(OSSCREEN_TV);
		OSScreenSetBufferEx(OSSCREEN_TV, MEMORY_MEM1_AREA_ADDR);
		OSScreenSetBufferEx(OSSCREEN_DRC, MEMORY_MEM1_AREA_ADDR + tvBufferSize);
		OSScreenEnableEx(OSSCREEN_TV, 1);
		OSScreenEnableEx(OSSCREEN_DRC, 1);

		while ( true )
		{
			OSScreenClearBufferEx(OSSCREEN_TV, 0);
			OSScreenClearBufferEx(OSSCREEN_DRC, 0);
			OSScreenPutFontEx(OSSCREEN_TV, 0, 0, msg);
			OSScreenPutFontEx(OSSCREEN_DRC, 0, 0, msg);
			OSScreenFlipBuffersEx(OSSCREEN_TV);
			OSScreenFlipBuffersEx(OSSCREEN_DRC);
			OSSleepTicks(ESPRESSO_TIMER_CLOCK / 30);
		}
	}

	/* home button menu */

	bool g_homeButtonMenuEnabled = false;
	// Whether the game has said either way. It starts out false above, which
	// on its own would read as every game refusing the HOME Menu; only a game
	// that turned it off is refusing it.
	std::atomic<bool> s_homeButtonMenuSetByGame = false;

	bool OSIsHomeButtonMenuEnabled()
	{
		return g_homeButtonMenuEnabled;
	}

	bool OSEnableHomeButtonMenu(bool enable)
	{
		// Said when it changes: whether Dualscreen's menu can open turns on
		// this, and a game that turns it off for good would refuse it always.
		if (enable != g_homeButtonMenuEnabled || !s_homeButtonMenuSetByGame)
			cemuLog_log(LogType::Force, "Dualscreen: the game turned its HOME Menu {}", enable ? "on" : "off");
		g_homeButtonMenuEnabled = enable;
		s_homeButtonMenuSetByGame = true;
		return true;
	}

	uint32 OSGetPFID()
	{
		return 15; // hardcoded as game
	}

	uint32 OSGetUPID()
	{
		return OSGetPFID();
	}

	uint64 s_currentTitleId;

	uint64 OSGetTitleID()
	{
		return s_currentTitleId;
	}

	uint32 s_sdkVersion;

	uint32 __OSGetProcessSDKVersion()
	{
		return s_sdkVersion;
	}

	void OSLauncherThread(uint64 titleId)
	{
		CafeSystem::ShutdownTitle();
		CafeSystem::PrepareForegroundTitle(titleId);
		CafeSystem::RequestRecreateCanvas();
		CafeSystem::LaunchForegroundTitle();
	}

	void OSShutdownThread(sint32 status)
	{
		CafeSystem::ShutdownTitle();
		CafeSystem::NotifyPPCProcessExit(status);
	}

	uint32 __LaunchByTitleId(uint64 titleId, uint32 argc, MEMPTR<char>* argv)
	{
		// prepare argument buffer
		#if 0
		char argumentBuffer[4096];
		uint32 argumentBufferLength = 0;
		char* argWriter = argumentBuffer;
		for(uint32 i=0; i<argc; i++)
		{
			const char* arg = argv[i];
			uint32 argLength = strlen(arg);
			if((argumentBufferLength + argLength + 1) >= sizeof(argumentBuffer))
			{
				// argument buffer full
				cemuLog_logDebug(LogType::Force, "LaunchByTitleId: argument buffer full");
				return 0x80000000;
			}
			memcpy(argWriter, arg, argLength);
			argWriter[argLength] = '\0';
			argWriter += argLength + 1;
			argumentBufferLength += argLength + 1;
		}
		#endif
		// normally the above buffer is passed to the PPC kernel via syscall 0x2B and then
		// the kernel forwards it to IOSU MCP when requesting a title launch
		// but for now we HLE most of the launching code and can just set the argument array directly
		std::vector<std::string> argArray;
		for(uint32 i=0; i<argc; i++)
			argArray.emplace_back(argv[i]);
		CafeSystem::SetOverrideArgs(argArray);
		// spawn launcher thread (this current thread will be destroyed during relaunch)
		std::thread launcherThread(OSLauncherThread, titleId);
		launcherThread.detach();
		// suspend this thread
		coreinit::OSSuspendThread(coreinit::OSGetCurrentThread());
		return 0;
	}

	uint32 OSLaunchTitleByPathl(const char* path, uint32 pathLength, uint32 argc)
	{
		char appXmlPath[1024];
		cemu_assert_debug(argc == 0); // custom args not supported yet
		if(pathLength >= (sizeof(appXmlPath) - 32))
		{
			// path too long
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: path too long");
			return 0x80000000;
		}
		// read app.xml to get the titleId
		memcpy(appXmlPath, path, pathLength);
		appXmlPath[pathLength] = '\0';
		strcat(appXmlPath, "/code/app.xml");
		sint32 status;
		auto fscfile = fsc_open(appXmlPath, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::READ_PERMISSION, &status);
		if (!fscfile)
		{
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: failed to open target app.xml");
			return 0x80000000;
		}
		uint32 size = fsc_getFileSize(fscfile);
		std::vector<uint8> tmpData(size);
		fsc_readFile(fscfile, tmpData.data(), size);
		fsc_close(fscfile);
		// parse app.xml to get the titleId
		pugi::xml_document app_doc;
		if (!app_doc.load_buffer_inplace(tmpData.data(), tmpData.size()))
			return false;
		uint64 titleId = std::stoull(app_doc.child("app").child("title_id").child_value(), nullptr, 16);
		if(titleId == 0)
		{
			cemuLog_logDebug(LogType::Force, "OSLaunchTitleByPathl: failed to parse titleId from app.xml");
			return 0x80000000;
		}
		__LaunchByTitleId(titleId, 0, nullptr);
		return 0;
	}

	uint32 OSRestartGame(uint32 argc, MEMPTR<char>* argv)
	{
		__LaunchByTitleId(CafeSystem::GetForegroundTitleId(), argc, argv);
		return 0;
	}

	void OSReleaseForeground()
	{
		cemuLog_logDebug(LogType::Force, "OSReleaseForeground not implemented");
		// Games that skip ProcUI call this themselves once they have stopped.
		DualscreenForegroundReleased();
	}

	// Atomic because Dualscreen's requests arrive on the GPU thread, while
	// UpdateSystemMessageQueue() runs on whichever core asks for a message.
	std::atomic<bool> s_transitionToBackground = false;
	std::atomic<bool> s_transitionToForeground = false;
	std::atomic<bool> s_sendHomeButtonDenied = false;
	// When the game last asked for a system message, in milliseconds on the
	// steady clock; 0 for never. A game that handles the HOME Menu asks every
	// frame. One that has not asked for a second will not hear it is to
	// pause either -- Affordable Space Adventures asks once, while it boots,
	// and never again -- so it is not left waiting for an answer.
	std::atomic<uint64> s_systemMessagesReadAtMs = 0;

	uint64 SteadyMs()
	{
		return (uint64)std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// Dualscreen's quick menu pauses a game the way the Wii U's HOME Menu
	// does: the game is told to release the foreground, stops itself at a
	// safe point, and waits until told to acquire it again. Cemu cannot stop
	// a game from outside, so this is the only pause that leaves it in a
	// state it expects.
	//
	// Releasing: the release message is queued or delivered, and the game
	// has yet to let go. Background: it has, and is waiting.
	enum class DualscreenPause
	{
		None,
		Releasing,
		Background,
	};
	std::mutex s_dualscreenPauseMutex;
	DualscreenPause s_dualscreenPause = DualscreenPause::None;
	// Dualscreen asked for the game back before it had finished letting go.
	bool s_dualscreenResumeWanted = false;

	void SendAcquireForeground()
	{
		OSMessage msg{};
		msg.data0 = stdx::to_underlying(SysMessageId::MsgAcquireForeground);
		msg.data1 = 1; // ?
		msg.data2 = 1; // ?
		// Straight onto the queue rather than through s_transitionToForeground:
		// a game in the background is already blocked receiving, and that flag
		// is only looked at when a receive begins.
		if (!OSSendMessage(coreinit::OSGetSystemMessageQueue(), &msg, 0))
			cemuLog_log(LogType::Force, "Dualscreen: the system message queue is full; the game was not told to go on");
	}

	void DualscreenRequestBackground()
	{
		std::scoped_lock lock(s_dualscreenPauseMutex);
		if (s_dualscreenPause != DualscreenPause::None)
			return;
		const uint64 readAt = s_systemMessagesReadAtMs;
		if (readAt == 0 || SteadyMs() - readAt > 1000)
		{
			cemuLog_log(LogType::Force, "Dualscreen: the game doesn't listen for the HOME Menu, so it can't be paused");
			DualscreenSession::instance().ReportPauseState(ds::ExternalPauseState::NotListening);
			return;
		}
		if (s_homeButtonMenuSetByGame && !g_homeButtonMenuEnabled)
		{
			// What the console does while a game has the HOME Menu turned
			// off, which games do while saving.
			s_sendHomeButtonDenied = true;
			cemuLog_log(LogType::Force, "Dualscreen: the game can't be paused right now");
			DualscreenSession::instance().ReportPauseState(ds::ExternalPauseState::Refused);
			return;
		}
		s_dualscreenPause = DualscreenPause::Releasing;
		s_dualscreenResumeWanted = false;
		s_transitionToBackground = true;
		cemuLog_log(LogType::Force, "Dualscreen: asking the game to pause");
	}

	void DualscreenRequestForeground()
	{
		std::scoped_lock lock(s_dualscreenPauseMutex);
		if (s_dualscreenPause == DualscreenPause::Releasing)
		{
			if (s_transitionToBackground.exchange(false))
			{
				// The game never picked the message up, so it never stopped.
				s_dualscreenPause = DualscreenPause::None;
				cemuLog_log(LogType::Force, "Dualscreen: the game never paused; the request was withdrawn");
				DualscreenSession::instance().ReportPauseState(ds::ExternalPauseState::Running);
				return;
			}
			// It has the message and is stopping. Telling it to go on now could
			// arrive before it lets go, leaving it in the background with
			// nothing left to bring it back, so that waits for it to finish.
			s_dualscreenResumeWanted = true;
			return;
		}
		if (s_dualscreenPause == DualscreenPause::Background)
		{
			cemuLog_log(LogType::Force, "Dualscreen: letting the game go on");
			SendAcquireForeground();
		}
	}

	void DualscreenForegroundReleased()
	{
		std::scoped_lock lock(s_dualscreenPauseMutex);
		// Also reached by ProcUI's core threads, one per core, and by
		// transitions Dualscreen did not ask for.
		if (s_dualscreenPause != DualscreenPause::Releasing)
			return;
		s_dualscreenPause = DualscreenPause::Background;
		cemuLog_log(LogType::Force, "Dualscreen: the game paused");
		DualscreenSession::instance().ReportPauseState(ds::ExternalPauseState::Paused);
		if (s_dualscreenResumeWanted)
		{
			s_dualscreenResumeWanted = false;
			cemuLog_log(LogType::Force, "Dualscreen: letting the game go on");
			SendAcquireForeground();
		}
	}

	void DualscreenForegroundAcquired()
	{
		std::scoped_lock lock(s_dualscreenPauseMutex);
		if (s_dualscreenPause != DualscreenPause::Background)
			return;
		s_dualscreenPause = DualscreenPause::None;
		cemuLog_log(LogType::Force, "Dualscreen: the game is going again");
		DualscreenSession::instance().ReportPauseState(ds::ExternalPauseState::Running);
	}

	void StartBackgroundForegroundTransition()
	{
		s_transitionToBackground = true;
		s_transitionToForeground = true;
	}

	// called at the beginning of OSReceiveMessage if the queue is the system message queue
	void UpdateSystemMessageQueue()
	{
		s_systemMessagesReadAtMs = SteadyMs();
		if(!OSIsInterruptEnabled())
			return;
		cemu_assert_debug(!__OSHasSchedulerLock());
		// normally syscall 0x2E is used to get the next message
		// for now we just have some preliminary logic here to allow a fake transition to background & foreground
		if(s_sendHomeButtonDenied.exchange(false))
		{
			OSMessage msg{};
			msg.data0 = stdx::to_underlying(SysMessageId::HomeButtonDenied);
			if(!OSSendMessage(coreinit::OSGetSystemMessageQueue(), &msg, 0))
				s_sendHomeButtonDenied = true;
			return;
		}
		// Taken rather than read, so a Dualscreen request withdrawn at the
		// same moment is either sent here or withdrawn there, never both.
		if(s_transitionToBackground.exchange(false))
		{
			// add transition to background message
			OSMessage msg{};
			msg.data0 = stdx::to_underlying(SysMessageId::MsgReleaseForeground);
			msg.data1 = 0; // 1 -> System is shutting down 0 -> Begin transitioning to background
			OSMessageQueue* systemMessageQueue = coreinit::OSGetSystemMessageQueue();
			if(!OSSendMessage(systemMessageQueue, &msg, 0))
				s_transitionToBackground = true;
			return;
		}
		if(s_transitionToForeground)
		{
			// add transition to foreground message
			OSMessage msg{};
			msg.data0 = stdx::to_underlying(SysMessageId::MsgAcquireForeground);
			msg.data1 = 1; // ?
			msg.data2 = 1; // ?
			OSMessageQueue* systemMessageQueue = coreinit::OSGetSystemMessageQueue();
			if(OSSendMessage(systemMessageQueue, &msg, 0))
				s_transitionToForeground = false;
			return;
		}
	}

	// called when OSReceiveMessage returns a message from the system message queue
	void HandleReceivedSystemMessage(OSMessage* msg)
	{
		cemu_assert_debug(!__OSHasSchedulerLock());
		cemuLog_log(LogType::Force, "Receiving message: {:08x}", (uint32)msg->data0);
		if ((uint32)msg->data0 == stdx::to_underlying(SysMessageId::MsgAcquireForeground))
			DualscreenForegroundAcquired();
	}

	uint32 OSDriver_Register(uint32 moduleHandle, sint32 priority, OSDriverInterface* driverCallbacks, sint32 driverId, uint32be* outUkn1, uint32be* outUkn2, uint32be* outUkn3)
	{
		cemuLog_logDebug(LogType::Force, "OSDriver_Register stubbed");
		return 0;
	}

	uint32 OSDriver_Deregister(uint32 moduleHandle, sint32 driverId)
	{
		cemuLog_logDebug(LogType::Force, "OSDriver_Deregister stubbed");
		return 0;
	}

	void __PPCExit(sint32 status)
	{
		// spawn shutdown thread (the current thread has to be destroyed as part of the shutdown process)
		std::thread shutdownThread(OSShutdownThread, status);
		shutdownThread.detach();
		// suspend this thread
		coreinit::OSSuspendThread(coreinit::OSGetCurrentThread());
	}

	void coreinit_exit(sint32 status)
	{
		cemuLog_log(LogType::Force, "The title terminated the process by calling coreinit.exit({})", (sint32)status);
		DebugLogStackTrace(coreinit::OSGetCurrentThread(), coreinit::OSGetStackPointer());
		if (gCoreinitData->__atexit_cleanup)
			PPCCoreCallback(gCoreinitData->__atexit_cleanup, status);
		if (gCoreinitData->__stdio_cleanup)
			PPCCoreCallback(gCoreinitData->__stdio_cleanup);
		__PPCExit(status);
	}

	void miscInit()
	{
		s_currentTitleId = CafeSystem::GetForegroundTitleId();
		s_sdkVersion = CafeSystem::GetForegroundTitleSDKVersion();
		s_forwardConsoleLogs = LaunchSettings::ForwardConsoleLogging();
		s_transitionToBackground = false;
		s_transitionToForeground = false;
		s_sendHomeButtonDenied = false;
		s_homeButtonMenuSetByGame = false;
		s_systemMessagesReadAtMs = 0;
		{
			std::scoped_lock lock(s_dualscreenPauseMutex);
			s_dualscreenPause = DualscreenPause::None;
			s_dualscreenResumeWanted = false;
		}

		cafeExportRegister("coreinit", __os_snprintf, LogType::Placeholder);

		cafeExportRegister("coreinit", COSVReport, LogType::Placeholder);
		cafeExportRegister("coreinit", COSWarn, LogType::Placeholder);
		cafeExportRegister("coreinit", OSReport, LogType::Placeholder);
		cafeExportRegister("coreinit", OSVReport, LogType::Placeholder);
		cafeExportRegister("coreinit", OSLogPrintf, LogType::Placeholder);
		cafeExportRegister("coreinit", OSConsoleWrite, LogType::Placeholder);
		cafeExportRegister("coreinit", OSFatal, LogType::Placeholder);

		cafeExportRegister("coreinit", OSGetPFID, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetUPID, LogType::Placeholder);
		cafeExportRegister("coreinit", OSGetTitleID, LogType::Placeholder);
		cafeExportRegister("coreinit", __OSGetProcessSDKVersion, LogType::Placeholder);

		g_homeButtonMenuEnabled = true; // enabled by default
		// Disney Infinity 2.0 actually relies on home button menu being enabled by default. If it's false it will crash due to calling erreula->IsAppearHomeNixSign() before initializing erreula
		cafeExportRegister("coreinit", OSIsHomeButtonMenuEnabled, LogType::CoreinitThread);
		cafeExportRegister("coreinit", OSEnableHomeButtonMenu, LogType::CoreinitThread);

		cafeExportRegister("coreinit", OSLaunchTitleByPathl, LogType::Placeholder);
		cafeExportRegister("coreinit", OSRestartGame, LogType::Placeholder);

		cafeExportRegister("coreinit", OSReleaseForeground, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDriver_Register, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDriver_Deregister, LogType::Placeholder);

		cafeExportRegisterFunc(coreinit_exit, "coreinit", "exit", LogType::CoreinitThread);
		cafeExportRegister("coreinit", __PPCExit, LogType::CoreinitThread);
	}

};
