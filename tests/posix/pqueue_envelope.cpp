#include "pqueue/envelope.h"

#include "doctest/doctest.h"

#include <string>

TEST_CASE("pqueue envelope round trips binary payload") {
    std::string payload;
    payload.push_back('a');
    payload.push_back('\0');
    payload.push_back(static_cast<char>(0xff));
    payload.push_back('z');

    std::string record;
    REQUIRE(pqueue::envelope::encodeEnvelope(3, payload, record));

    pqueue::envelope::DecodedEnvelope decoded;
    REQUIRE(pqueue::envelope::decodeEnvelope(record, decoded));
    CHECK_EQ(decoded.attempts, 3U);
    CHECK_EQ(decoded.payload, payload);
}

TEST_CASE("pqueue envelope rejects corrupted records") {
    std::string record;
    REQUIRE(pqueue::envelope::encodeEnvelope(1, "payload", record));
    REQUIRE(!record.empty());

    record[record.size() / 2] ^= 0x01;

    pqueue::envelope::DecodedEnvelope decoded;
    CHECK_FALSE(pqueue::envelope::decodeEnvelope(record, decoded));
}

TEST_CASE("pqueue envelope rejects truncated records") {
    std::string record;
    REQUIRE(pqueue::envelope::encodeEnvelope(1, "payload", record));
    record.resize(record.size() - 1);

    pqueue::envelope::DecodedEnvelope decoded;
    CHECK_FALSE(pqueue::envelope::decodeEnvelope(record, decoded));
}
