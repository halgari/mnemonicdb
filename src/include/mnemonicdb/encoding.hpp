#pragma once

#include <cstdint>
#include <string>

namespace mnemonicdb {

//! Append the low `width` bytes of `value` in big-endian order
inline void PutBigEndian(std::string &out, uint64_t value, int width) {
	for (int i = width - 1; i >= 0; i--) {
		out.push_back(static_cast<char>((value >> (i * 8)) & 0xFF));
	}
}

//! Read `width` big-endian bytes starting at `data`
inline uint64_t GetBigEndian(const char *data, int width) {
	uint64_t value = 0;
	for (int i = 0; i < width; i++) {
		value = (value << 8) | static_cast<uint8_t>(data[i]);
	}
	return value;
}

} // namespace mnemonicdb
