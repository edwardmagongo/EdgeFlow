#include <gtest/gtest.h>
#include "edgeflow/args.hpp"

TEST(ArgValue, ExtractsValueAfterEquals) {
    EXPECT_EQ(edgeflow::arg_value("--port=8080"), "8080");
}

TEST(ArgValue, ThrowsWithoutEquals) {
    EXPECT_THROW(edgeflow::arg_value("--port"), std::invalid_argument);
}
