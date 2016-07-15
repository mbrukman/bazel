// Copyright 2016 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BAZEL_SRC_TOOLS_SINGLEJAR_INPUT_JAR_SCAN_ENTRIES_TEST_H_
#define BAZEL_SRC_TOOLS_SINGLEJAR_INPUT_JAR_SCAN_ENTRIES_TEST_H_ 1

#include <errno.h>
#include <unistd.h>
#include <memory>
#include <string>

#include "src/tools/singlejar/input_jar.h"

#include "gtest/gtest.h"

static const char kJar[] = "jar.jar";
static const char kXXXX[] = "4GB-1file";
static const char kEmpty[] = "empty";
static const char kRes1[] = "res1";
static const char kRes2[] = "res2";
static const char kHuge[] = "4GB+1file";
static const int32_t res1_size = 123;
static const int32_t res2_size = 456;
static const int64_t huge_size = 0x100000001L;
static const int64_t kHugeOffset = 0x100000001L;

/* Verifies that InputJar can handle zip/jar files created by a given creator.
 * This includes basic directory scan, handling huge (>4GB) zip files and huge
 * entries in them, and handling zip files with "huge" (>64K) number of entries.
 * A creator is passed as a typed parameter.
 */
template <class ZipCreator>
class InputJarScanEntries : public testing::Test {
 public:
  static void SetUpTestCase() { ZipCreator::SetUpTestCase(); }

  static void TearDownTestCase() { ZipCreator::TearDownTestCase(); }

  // Allocates a with given name and with given size.
  static bool AllocateFile(const char *name, size_t size) {
    int fd = open(name, O_CREAT | O_RDWR | O_TRUNC, 0777);
    if (fd < 0) {
      perror(name);
      return false;
    }
    if (size) {
      if (ftruncate(fd, size) == 0) {
        return close(fd) == 0;
      } else {
        auto last_error = errno;
        close(fd);
        errno = last_error;
        return false;
      }
    } else {
      return close(fd) == 0;
    }
  }

  static void CreateBasicJar() {
    ASSERT_TRUE(AllocateFile(kRes1, res1_size));
    ASSERT_TRUE(AllocateFile(kRes2, res2_size));
    unlink(kJar);
    ASSERT_EQ(0, ZipCreator::Jar(true, kJar, kRes1, kRes2, nullptr));
    unlink(kRes1);
    unlink(kRes2);
  }

  static void CreateJarWithHugeUncompressed() {
    ASSERT_TRUE(AllocateFile(kHuge, huge_size));
    unlink(kJar);
    ASSERT_EQ(0, ZipCreator::Jar(true, kJar, kHuge, nullptr));
    unlink(kHuge);
  }

  static void CreateJarWithZip64Entries() {
    ASSERT_TRUE(AllocateFile(kXXXX, 0xFFFFFFFF));
    ASSERT_TRUE(AllocateFile(kHuge, huge_size));
    ASSERT_TRUE(AllocateFile(kEmpty, 0));
    ASSERT_TRUE(AllocateFile(kRes1, res1_size));
    ASSERT_EQ(
        0, ZipCreator::Jar(false, kJar, kXXXX, kHuge, kEmpty, kRes1, nullptr));
    unlink(kXXXX);
    unlink(kHuge);
    unlink(kEmpty);
    unlink(kRes1);
  }

  static void CreateJarWithLotsOfEntries() {
    unlink(kJar);
    // Create 256 directories with 256 files in each one,
    // make an archive from them
    for (int dir = 0; dir < 256; ++dir) {
      char dirname[10];
      snprintf(dirname, sizeof(dirname), "dir%d", dir);
      ASSERT_EQ(0, mkdir(dirname, 0777));
      for (int file = 0; file < 256; ++file) {
        char filepath[20];
        snprintf(filepath, sizeof(filepath), "%s/%d", dirname, file);
        ASSERT_TRUE(AllocateFile(filepath, 1));
      }
    }
    ASSERT_EQ(0, ZipCreator::Jar(false, kJar, "dir*", nullptr));
    for (int dir = 0; dir < 256; ++dir) {
      char rmdircmd[100];
      snprintf(rmdircmd, sizeof(rmdircmd), "rm dir%d/* && rmdir dir%d", dir,
               dir);
      ASSERT_EQ(0, system(rmdircmd));
    }
  }

  static void LsZip(const char *zip_name) {
#if !defined(__APPLE__)
    std::string command = (std::string("unzip -v ") + zip_name).c_str();
    ASSERT_EQ(0, system(command.c_str())) << "Failed command: " << command;
#endif
  }

  void SetUp() override { input_jar_.reset(new InputJar); }

  static void SmogCheck(const CDH *cdh, const LH *lh) {
    ASSERT_TRUE(cdh->is()) << "No expected tag in the Central Directory Entry.";
    ASSERT_NE(nullptr, lh) << "No local header.";
    ASSERT_TRUE(lh->is()) << "No expected tag in the Local Header.";
    EXPECT_EQ(lh->file_name_length(), cdh->file_name_length());
    EXPECT_NE(lh->file_name_length(), 0);
    std::string lh_name(lh->file_name(), lh->file_name_length());
    std::string cdh_name(cdh->file_name(), cdh->file_name_length());
    EXPECT_EQ(lh_name, cdh_name);
    if (!cdh->no_size_in_local_header()) {
      EXPECT_EQ(lh->compressed_file_size(), cdh->compressed_file_size())
          << "Entry: " << lh_name;
      EXPECT_EQ(lh->uncompressed_file_size(), cdh->uncompressed_file_size())
          << "Entry: " << cdh_name;
    }
  }

  std::unique_ptr<InputJar> input_jar_;
};

TYPED_TEST_CASE_P(InputJarScanEntries);

TYPED_TEST_P(InputJarScanEntries, OpenClose) {
  ASSERT_EQ(0, chdir(getenv("TEST_TMPDIR")));
  this->CreateBasicJar();
  this->LsZip(kJar);
  ASSERT_TRUE(this->input_jar_->Open(kJar));
  EXPECT_GE(this->input_jar_->fd(), 0);
  this->input_jar_->Close();
  EXPECT_LT(this->input_jar_->fd(), 0);
}

/*
 * Check that the jar has the expected entries, they have expected
 * sizes, and that we can access both central directory entries and
 * local headers.
 */
TYPED_TEST_P(InputJarScanEntries, Basic) {
  ASSERT_EQ(0, chdir(getenv("TEST_TMPDIR")));
  this->CreateBasicJar();
  ASSERT_TRUE(this->input_jar_->Open(kJar));
  const LH *lh;
  const CDH *cdh;
  int file_count = 0;
  bool res1_present = false;
  bool res2_present = false;
  for (int entry_count = 0; (cdh = this->input_jar_->NextEntry(&lh));
       ++entry_count) {
    this->SmogCheck(cdh, lh);
    if ('/' != lh->file_name()[lh->file_name_length() - 1]) {
      ++file_count;
      if (cdh->file_name_is(kRes1)) {
        EXPECT_EQ(res1_size, cdh->uncompressed_file_size());
        res1_present = true;
      } else if (cdh->file_name_is(kRes2)) {
        EXPECT_EQ(res2_size, cdh->uncompressed_file_size());
        res2_present = true;
      }
    }
  }

  this->input_jar_->Close();
  unlink(kJar);
  EXPECT_TRUE(res1_present) << "Jar file " << kJar << " lacks expected '"
                            << kRes1 << "' file.";
  EXPECT_TRUE(res2_present) << "Jar file " << kJar << " lacks expected '"
                            << kRes2 << "' file.";
}

/*
 * Check we can handle >4GB jar with >4GB entry in it.
 */
TYPED_TEST_P(InputJarScanEntries, HugeUncompressed) {
  ASSERT_EQ(0, chdir(getenv("TEST_TMPDIR")));
  this->CreateJarWithHugeUncompressed();
  this->LsZip(kJar);
  ASSERT_TRUE(this->input_jar_->Open(kJar));
  const LH *lh;
  const CDH *cdh;
  bool huge_file_present = false;

  while ((cdh = this->input_jar_->NextEntry(&lh))) {
    this->SmogCheck(cdh, lh);
    if (cdh->file_name_is(kHuge)) {
      std::string entry_name(cdh->file_name(), cdh->file_name_length());
      EXPECT_EQ(huge_size, cdh->uncompressed_file_size()) << "Entry: "
                                                          << entry_name;
      huge_file_present = true;
    }
  }
  this->input_jar_->Close();
  unlink(kJar);
  EXPECT_TRUE(huge_file_present) << "Jar file " << kJar << " lacks expected '"
                                 << kHuge << "' file.";
}

/*
 * Check we can handle >4GB jar with huge and small entries and huge and
 * small offsets in the central directory.
 */
TYPED_TEST_P(InputJarScanEntries, TestZip64) {
  ASSERT_EQ(0, chdir(getenv("TEST_TMPDIR")));
  this->CreateJarWithZip64Entries();
  this->LsZip(kJar);
  ASSERT_TRUE(this->input_jar_->Open(kJar));
  const LH *lh;
  const CDH *cdh;
  while ((cdh = this->input_jar_->NextEntry(&lh))) {
    this->SmogCheck(cdh, lh);

    if (cdh->file_name_is(kXXXX)) {
      EXPECT_EQ(0xFFFFFFFF, cdh->uncompressed_file_size());
      EXPECT_EQ(0xFFFFFFFF, cdh->compressed_file_size());
    } else if (cdh->file_name_is(kHuge)) {
      EXPECT_EQ(huge_size, cdh->uncompressed_file_size());
      EXPECT_EQ(huge_size, cdh->compressed_file_size());
      EXPECT_LT(kHugeOffset, cdh->local_header_offset());
    } else if (cdh->file_name_is(kEmpty)) {
      EXPECT_EQ(0, cdh->uncompressed_file_size());
      EXPECT_EQ(0, cdh->compressed_file_size());
      EXPECT_EQ(0, lh->compressed_file_size());
      EXPECT_LT(kHugeOffset, cdh->local_header_offset());
    } else if (cdh->file_name_is(kRes1)) {
      EXPECT_EQ(res1_size, cdh->uncompressed_file_size());
      EXPECT_LT(kHugeOffset, cdh->local_header_offset());
    }
  }
  this->input_jar_->Close();
  unlink(kJar);
}

/*
 * Check we can handle >64K entries.
 */
TYPED_TEST_P(InputJarScanEntries, LotsOfEntries) {
  ASSERT_EQ(0, chdir(getenv("TEST_TMPDIR")));
  this->CreateJarWithLotsOfEntries();
#if !defined(__APPLE__)
  const char kTailUnzip[] = "unzip -v jar.jar | tail";
  ASSERT_EQ(0, system(kTailUnzip)) << "Failed command: " << kTailUnzip;
#endif
  ASSERT_TRUE(this->input_jar_->Open(kJar));
  const LH *lh;
  const CDH *cdh;
  int entry_count = 0;
  int file_count = 0;
  int dir_count = 0;
  while ((cdh = this->input_jar_->NextEntry(&lh))) {
    this->SmogCheck(cdh, lh);
    ++entry_count;
    if (cdh->file_name()[cdh->file_name_length() - 1] == '/') {
      ++dir_count;
    } else {
      ++file_count;
    }
  }
  this->input_jar_->Close();
  unlink(kJar);

  /* We cannot compare to the exact number because JDK's jar
   * adds META-INF/ and META-INF/MANIFEST.MF.
   */
  EXPECT_LE(256 * 257, entry_count);
  EXPECT_LE(256, dir_count);
  EXPECT_LE(256 * 256, file_count);
}

REGISTER_TYPED_TEST_CASE_P(InputJarScanEntries, OpenClose, Basic,
                           HugeUncompressed, TestZip64, LotsOfEntries);

#endif  // BAZEL_SRC_TOOLS_SINGLEJAR_INPUT_JAR_SCAN_ENTRIES_TEST_H_