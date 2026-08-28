#include <gtest/gtest.h>
#include "edgeflow/args.hpp"

TEST(ArgValue, ExtractsValueAfterEquals) {
    EXPECT_EQ(edgeflow::arg_value("--port=8080"), "8080");
}

TEST(ArgValue, ThrowsWithoutEquals) {
    EXPECT_THROW(edgeflow::arg_value("--port"), std::invalid_argument);
}

TEST(ParsePercentage, AcceptsValidRange) {
    EXPECT_DOUBLE_EQ(edgeflow::parse_percentage("0", "--chaos-packet-loss-percent"), 0.0);
    EXPECT_DOUBLE_EQ(edgeflow::parse_percentage("50", "--chaos-packet-loss-percent"), 50.0);
    EXPECT_DOUBLE_EQ(edgeflow::parse_percentage("100", "--chaos-packet-loss-percent"), 100.0);
}

TEST(ParsePercentage, RejectsNegative) {
    EXPECT_THROW(edgeflow::parse_percentage("-1", "--chaos-packet-loss-percent"), std::invalid_argument);
}

TEST(ParsePercentage, RejectsAboveHundred) {
    EXPECT_THROW(edgeflow::parse_percentage("100.1", "--chaos-packet-loss-percent"), std::invalid_argument);
}

TEST(ParsePercentage, RejectsTrailingJunk) {
    EXPECT_THROW(edgeflow::parse_percentage("50junk", "--chaos-packet-loss-percent"), std::invalid_argument);
}

TEST(ParsePercentage, RejectsNonFinite) {
    EXPECT_THROW(edgeflow::parse_percentage("inf", "--chaos-packet-loss-percent"), std::invalid_argument);
    EXPECT_THROW(edgeflow::parse_percentage("nan", "--chaos-packet-loss-percent"), std::invalid_argument);
}
