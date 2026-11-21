#include <gtest/gtest.h>

#include "protocol/HttpMessage.hpp"
#include "util/Bytes.hpp"

using namespace veloplay;
using namespace veloplay::protocol;

TEST(HttpMessage, ParsesStatusLineAndHeaders) {
    const auto response = parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 12\r\nX-Empty:\r\n\r\nbody");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->reason, "OK");
    EXPECT_EQ(response->headers.get("content-length"), "12");
    EXPECT_EQ(response->headers.get("X-Empty"), "");
    EXPECT_FALSE(response->headers.get("body"));
}

TEST(HttpMessage, AcceptsBareLineFeedsAndNoSpaceAfterColon) {
    const auto response = parseHttpResponse("HTTP/1.1 620 Server Standby\nhost-id:AABBCCDDEEFF\nhost-name:My PS5\n");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 620);
    EXPECT_EQ(response->reason, "Server Standby");
    EXPECT_EQ(response->headers.get("host-id"), "AABBCCDDEEFF");
    EXPECT_EQ(response->headers.get("host-name"), "My PS5");
}

TEST(HttpMessage, StopsAtNulByte) {
    const std::string_view text("HTTP/1.1 200 Ok\na:1\n\0b:2\n", 24);
    const auto response = parseHttpResponse(text);
    ASSERT_TRUE(response);
    EXPECT_TRUE(response->headers.get("a"));
    EXPECT_FALSE(response->headers.get("b"));
}

TEST(HttpMessage, RejectsGarbage) {
    EXPECT_FALSE(parseHttpResponse(""));
    EXPECT_FALSE(parseHttpResponse("SRCH * HTTP/1.1\n"));
    EXPECT_FALSE(parseHttpResponse("HTTP/1.1 abc\n"));
    EXPECT_FALSE(parseHttpResponse("HTTP/1.1 7\n"));
}

TEST(HttpMessage, HeaderValuesMayContainColons) {
    const HttpHeaders headers = parseHeaderLines("AP-Bssid: 00:11:22:33:44:55\r\nbroken line\r\nKey: v\r\n");
    EXPECT_EQ(headers.get("AP-Bssid"), "00:11:22:33:44:55");
    EXPECT_EQ(headers.get("Key"), "v");
    EXPECT_EQ(headers.entries().size(), 2u);
}

TEST(HttpMessage, FindsEndOfHeaderBlock) {
    const std::string text = "HTTP/1.1 200 OK\r\nA: b\r\n\r\nBODY";
    const auto end = findHeaderEnd(asBytes(text));
    ASSERT_TRUE(end);
    EXPECT_EQ(text.substr(*end), "BODY");
    EXPECT_FALSE(findHeaderEnd(asBytes(std::string_view("HTTP/1.1 200 OK\r\nA: b\r\n"))));
}
