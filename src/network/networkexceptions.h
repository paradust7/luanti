// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013-2017 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "exceptions.h"

namespace con
{
/*
	Exceptions
*/

class PeerNotFoundException : public BaseException
{
public:
	PeerNotFoundException(const char *s) : BaseException(s) {}
};

class ConnectionException : public BaseException
{
public:
	ConnectionException(const char *s) : BaseException(s) {}
};

class InvalidIncomingDataException : public BaseException
{
public:
	InvalidIncomingDataException(const char *s) : BaseException(s) {}
};

}

class SocketException : public BaseException
{
public:
	SocketException(const std::string &s) : BaseException(s) {}
};

// The address to listen on is IPv6, but IPv6 is disabled
class IPv6DisabledException : public SocketException
{
public:
	IPv6DisabledException(const std::string &address) :
		SocketException("Unable to listen on " + address + " because IPv6 is disabled"),
		address(address) {}

	std::string address;
};

class ResolveError : public BaseException
{
public:
	ResolveError(const std::string &s) : BaseException(s) {}
};

class SendFailedException : public BaseException
{
public:
	SendFailedException(const std::string &s) : BaseException(s) {}
};
