#include <fcntl.h>

#include "base/file.h"
#include "storage/ca-table/ca-table.h"
#include "third_party/gtest/gtest.h"

struct WriteOnceTest : testing::Test {
 public:
  void SetUp() override {
    temp_directory_ =
        std::make_unique<ev::DirectoryTreeRemover>(ev::TemporaryDirectory());
  }

 protected:
  std::unique_ptr<ev::DirectoryTreeRemover> temp_directory_;
};

TEST_F(WriteOnceTest, CanWriteThenRead) {
  auto table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(),
      O_CREAT | O_TRUNC | O_WRONLY);
  table_handle->InsertRow("a", "xxx");
  table_handle->InsertRow("b", "yyy");
  table_handle->InsertRow("c", "zzz");
  table_handle->InsertRow("d", "www");
  table_handle->Sync();
  table_handle.reset();

  table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(), O_RDONLY);
  EXPECT_TRUE(table_handle->IsSorted());
  EXPECT_TRUE(table_handle->SeekToKey("a"));
  EXPECT_FALSE(table_handle->SeekToKey("D"));
  EXPECT_TRUE(table_handle->SeekToKey("c"));
  EXPECT_FALSE(table_handle->SeekToKey("A"));
  EXPECT_FALSE(table_handle->SeekToKey("C"));
  EXPECT_FALSE(table_handle->SeekToKey("B"));
  EXPECT_TRUE(table_handle->SeekToKey("d"));
  EXPECT_TRUE(table_handle->SeekToKey("b"));
}

TEST_F(WriteOnceTest, CanWriteThenReadMany) {
  auto table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(),
      O_CREAT | O_TRUNC | O_WRONLY);
  char str[3];
  str[2] = 0;
  for (str[0] = 'a'; str[0] <= 'z'; ++str[0]) {
    for (str[1] = 'a'; str[1] <= 'z'; ++str[1]) {
      table_handle->InsertRow(str, "xxx");
    }
  }

  table_handle->Sync();
  table_handle.reset();

  table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(), O_RDONLY);
  EXPECT_TRUE(table_handle->IsSorted());

  for (str[0] = 'a'; str[0] <= 'z'; ++str[0]) {
    for (str[1] = 'a'; str[1] <= 'z'; ++str[1]) {
      EXPECT_EQ(1, table_handle->SeekToKey(str));
    }
  }
}

TEST_F(WriteOnceTest, CanWriteThenReadUnsorted) {
  auto table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(),
      O_CREAT | O_TRUNC | O_WRONLY);
  table_handle->InsertRow("a", "xxx");
  table_handle->InsertRow("c", "zzz");
  table_handle->InsertRow("d", "www");
  table_handle->InsertRow("b", "yyy");
  table_handle->Sync();
  table_handle.reset();

  table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(), O_RDONLY);
  EXPECT_FALSE(table_handle->IsSorted());
  EXPECT_TRUE(table_handle->SeekToKey("a"));
  EXPECT_FALSE(table_handle->SeekToKey("D"));
  EXPECT_TRUE(table_handle->SeekToKey("c"));
  EXPECT_FALSE(table_handle->SeekToKey("A"));
  EXPECT_FALSE(table_handle->SeekToKey("C"));
  EXPECT_FALSE(table_handle->SeekToKey("B"));
  EXPECT_TRUE(table_handle->SeekToKey("d"));
  EXPECT_TRUE(table_handle->SeekToKey("b"));
}

TEST_F(WriteOnceTest, EmptyTableOK) {
  auto table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(),
      O_CREAT | O_TRUNC | O_WRONLY);
  table_handle->Sync();
  table_handle.reset();

  table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(), O_RDONLY);
}

TEST_F(WriteOnceTest, UnsyncedTableNotWritten) {
  auto table_handle = ca_table_open(
      "write-once", (temp_directory_->Root() + "/table_00").c_str(),
      O_CREAT | O_TRUNC | O_WRONLY);
  table_handle.reset();

  ASSERT_THROW(
      ca_table_open("write-once",
                    (temp_directory_->Root() + "/table_00").c_str(), O_RDONLY),
      kj::Exception);
}
