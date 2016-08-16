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

#include <stdlib.h>

#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/port.h"
#include "src/main/cpp/util/strings.h"
#include "src/tools/singlejar/input_jar.h"
#include "src/tools/singlejar/options.h"
#include "src/tools/singlejar/output_jar.h"
#include "src/tools/singlejar/test_util.h"
#include "gtest/gtest.h"

#if !defined(JAR_TOOL_PATH)
#error "The path to jar tool has to be defined via -DJAR_TOOL_PATH="
#endif

namespace {

using singlejar_test_util::CreateTextFile;
using singlejar_test_util::GetEntryContents;
using singlejar_test_util::GetEntryContents;
using singlejar_test_util::OutputFilePath;
using singlejar_test_util::RunCommand;
using singlejar_test_util::VerifyZip;

using std::string;

#if !defined(DATA_DIR_TOP)
#define DATA_DIR_TOP
#endif

static bool HasSubstr(const string &s, const string &what) {
  return string::npos != s.find(what);
}

class OutputJarSimpleTest : public ::testing::Test {
 protected:
  void CreateOutput(const string &out_path, const char *first_arg...) {
    va_list ap;
    va_start(ap, first_arg);
    const char *args[100] = {"--output", out_path.c_str()};
    unsigned nargs = 2;
    fprintf(stderr, "Creation arguments: ");
    if (first_arg) {
      args[nargs++] = first_arg;
      fprintf(stderr, "%s", first_arg);
      while (nargs < arraysize(args)) {
        const char *arg = va_arg(ap, const char *);
        if (arg) {
          args[nargs++] = arg;
          fprintf(stderr, " %s", arg);
        } else {
          break;
        }
      }
      va_end(ap);
      ASSERT_GE(arraysize(args), nargs);
    }
    fprintf(stderr, "\n");
    options_.ParseCommandLine(nargs, args);
    ASSERT_EQ(0, output_jar_.Doit(&options_));
    EXPECT_EQ(0, VerifyZip(out_path));
  }

  OutputJar output_jar_;
  Options options_;
};

// No inputs at all.
TEST_F(OutputJarSimpleTest, Empty) {
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, nullptr);
  InputJar input_jar;
  ASSERT_TRUE(input_jar.Open(out_path));
  const LH *lh;
  const CDH *cdh;
  while ((cdh = input_jar.NextEntry(&lh))) {
    ASSERT_TRUE(cdh->is()) << "No expected tag in the Central Directory Entry.";
    ASSERT_NE(nullptr, lh) << "No local header.";
    ASSERT_TRUE(lh->is()) << "No expected tag in the Local Header.";
    EXPECT_EQ(lh->file_name_string(), cdh->file_name_string());
    if (!cdh->no_size_in_local_header()) {
      EXPECT_EQ(lh->compressed_file_size(), cdh->compressed_file_size())
          << "Entry: " << lh->file_name_string();
      EXPECT_EQ(lh->uncompressed_file_size(), cdh->uncompressed_file_size())
          << "Entry: " << cdh->file_name_string();
    }
    // Verify that each entry has a reasonable timestamp.
    EXPECT_EQ(lh->last_mod_file_date(), cdh->last_mod_file_date())
        << "Entry: " << lh->file_name_string();
    EXPECT_EQ(lh->last_mod_file_time(), cdh->last_mod_file_time())
        << "Entry: " << lh->file_name_string();
    uint16_t dos_time = lh->last_mod_file_time();
    uint16_t dos_date = lh->last_mod_file_date();

    // Current time, rounded to even number of seconds because MSDOS timestamp
    // does this, too.
    time_t now = (time(nullptr) + 1) & ~1;
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char now_time_str[50];
    strftime(now_time_str, sizeof(now_time_str), "%c", &tm_now);

    // Unpack MSDOS file timestamp. See the comment about its format in
    // output_jar.cc.
    struct tm tm;
    tm.tm_sec = (dos_time & 31) << 1;
    tm.tm_min = (dos_time >> 5) & 63;
    tm.tm_hour = (dos_time >> 11) & 31;
    tm.tm_mday = (dos_date & 31);
    tm.tm_mon = ((dos_date >> 5) & 15) - 1;
    tm.tm_year = ((dos_date >> 9) & 127) + 80;
    tm.tm_isdst = tm_now.tm_isdst;
    time_t entry_time = mktime(&tm);
    char entry_time_str[50];
    strftime(entry_time_str, sizeof(entry_time_str), "%c", &tm);

    // Without --normalize option all the entries should have reasonably
    // current timestamp (which we arbitrarily choose to be <5 minutes).
    EXPECT_GE(now, entry_time) << now_time_str << " vs. " << entry_time_str;
    EXPECT_LE(now, entry_time + 300) << now_time_str << " vs. "
                                     << entry_time_str;
  }
  input_jar.Close();
  string manifest = GetEntryContents(out_path, "META-INF/MANIFEST.MF");
  EXPECT_EQ(
      "Manifest-Version: 1.0\r\n"
      "Created-By: singlejar\r\n"
      "\r\n",
      manifest);
  string build_properties = GetEntryContents(out_path, "build-data.properties");
  EXPECT_PRED2(HasSubstr, build_properties, "build.target=");
}

// Source jars.
TEST_F(OutputJarSimpleTest, Source) {
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--sources",
               DATA_DIR_TOP "src/tools/singlejar/libtest1.jar",
               DATA_DIR_TOP "src/tools/singlejar/libtest2.jar", nullptr);
  InputJar input_jar;
  ASSERT_TRUE(input_jar.Open(out_path));
  const LH *lh;
  const CDH *cdh;
  int file_count = 0;
  while ((cdh = input_jar.NextEntry(&lh))) {
    ASSERT_TRUE(cdh->is()) << "No expected tag in the Central Directory Entry.";
    ASSERT_NE(nullptr, lh) << "No local header.";
    ASSERT_TRUE(lh->is()) << "No expected tag in the Local Header.";
    EXPECT_EQ(lh->file_name_string(), cdh->file_name_string());
    if (!cdh->no_size_in_local_header()) {
      EXPECT_EQ(lh->compressed_file_size(), cdh->compressed_file_size())
          << "Entry: " << lh->file_name_string();
      EXPECT_EQ(lh->uncompressed_file_size(), cdh->uncompressed_file_size())
          << "Entry: " << cdh->file_name_string();
    }
    if (lh->file_name()[lh->file_name_length() - 1] != '/') {
      ++file_count;
    }
  }
  ASSERT_LE(4, file_count);
  input_jar.Close();
}

// Verify --java_launcher argument
TEST_F(OutputJarSimpleTest, JavaLauncher) {
  string out_path = OutputFilePath("out.jar");
  const char *launcher_path = DATA_DIR_TOP "src/tools/singlejar/libtest1.jar";
  CreateOutput(out_path, "--java_launcher", launcher_path, nullptr);
  // check that the offset of the first entry equals launcher size.
  InputJar input_jar;
  ASSERT_TRUE(input_jar.Open(out_path.c_str()));
  const LH *lh;
  const CDH *cdh;
  cdh = input_jar.NextEntry(&lh);
  ASSERT_NE(nullptr, cdh);
  struct stat statbuf;
  ASSERT_EQ(0, stat(launcher_path, &statbuf));
  EXPECT_TRUE(cdh->is());
  EXPECT_TRUE(lh->is());
  EXPECT_EQ(statbuf.st_size, cdh->local_header_offset());
  input_jar.Close();
}

// --main_class option.
TEST_F(OutputJarSimpleTest, MainClass) {
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--main_class", "com.google.my.Main", nullptr);
  string manifest = GetEntryContents(out_path, "META-INF/MANIFEST.MF");
  EXPECT_EQ(
      "Manifest-Version: 1.0\r\n"
      "Created-By: singlejar\r\n"
      "Main-Class: com.google.my.Main\r\n"
      "\r\n",
      manifest);
}

// --deploy_manifest_lines option.
TEST_F(OutputJarSimpleTest, DeployManifestLines) {
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--deploy_manifest_lines", "property1: foo",
               "property2: bar", nullptr);
  string manifest = GetEntryContents(out_path, "META-INF/MANIFEST.MF");
  EXPECT_EQ(
      "Manifest-Version: 1.0\r\n"
      "Created-By: singlejar\r\n"
      "property1: foo\r\n"
      "property2: bar\r\n"
      "\r\n",
      manifest);
}

// --extra_build_info option
TEST_F(OutputJarSimpleTest, ExtraBuildInfo) {
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--extra_build_info", "property1=value1",
               "--extra_build_info", "property2=value2", nullptr);
  string build_properties = GetEntryContents(out_path, "build-data.properties");
  EXPECT_PRED2(HasSubstr, build_properties, "\nproperty1=value1\n");
  EXPECT_PRED2(HasSubstr, build_properties, "\nproperty2=value2\n");
}

// --build_info_file and --extra_build_info options.
TEST_F(OutputJarSimpleTest, BuildInfoFile) {
  string build_info_path1 =
      CreateTextFile("buildinfo1", "property11=value11\nproperty12=value12\n");
  string build_info_path2 =
      CreateTextFile("buildinfo2", "property21=value21\nproperty22=value22\n");

  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--build_info_file", build_info_path1.c_str(),
               "--extra_build_info", "property=value", "--build_info_file",
               build_info_path2.c_str(), nullptr);
  string build_properties = GetEntryContents(out_path, "build-data.properties");
  EXPECT_PRED2(HasSubstr, build_properties, "property11=value11\n");
  EXPECT_PRED2(HasSubstr, build_properties, "property12=value12\n");
  EXPECT_PRED2(HasSubstr, build_properties, "property21=value21\n");
  EXPECT_PRED2(HasSubstr, build_properties, "property22=value22\n");
  EXPECT_PRED2(HasSubstr, build_properties, "property=value\n");
}

// --resources option.
TEST_F(OutputJarSimpleTest, Resources) {
  string res11_path = CreateTextFile("res11", "res11.line1\nres11.line2\n");
  string res11_spec = string("res1:") + res11_path;

  string res12_path = CreateTextFile("res12", "res12.line1\nres12.line2\n");
  string res12_spec = string("res1:") + res12_path;

  string res2_path = CreateTextFile("res2", "res2.line1\nres2.line2\n");

  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--resources", res11_spec.c_str(), res12_spec.c_str(),
               res2_path.c_str(), nullptr);

  // The output should have 'res1' entry containing the concatenation of the
  // 'res11' and 'res12' files.
  string res1 = GetEntryContents(out_path, "res1");
  EXPECT_EQ("res11.line1\nres11.line2\nres12.line1\nres12.line2\n", res1);

  // The output should have res2 path entry and contents.
  string res2 = GetEntryContents(out_path, res2_path);
  EXPECT_EQ("res2.line1\nres2.line2\n", res2);
}

// --classpath_resources
TEST_F(OutputJarSimpleTest, ClasspathResources) {
  string res1_path = OutputFilePath("cp_res");
  ASSERT_TRUE(blaze::WriteFile("line1\nline2\n", res1_path));
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--classpath_resources", res1_path.c_str(), nullptr);
  string res = GetEntryContents(out_path, "cp_res");
  EXPECT_EQ("line1\nline2\n", res);
}

// Duplicate entries for --resources or --classpath_resources
TEST_F(OutputJarSimpleTest, DuplicateResources) {
  string cp_res_path = CreateTextFile("cp_res", "line1\nline2\n");

  string res1_path = CreateTextFile("res1", "resline1\nresline2\n");
  string res1_spec = "foo:" + res1_path;

  string res2_path = CreateTextFile("res2", "line3\nline4\n");
  string res2_spec = "foo:" + res2_path;

  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--warn_duplicate_resources", "--resources",
               res1_spec.c_str(), res2_spec.c_str(), "--classpath_resources",
               cp_res_path.c_str(), cp_res_path.c_str(), nullptr);

  string cp_res = GetEntryContents(out_path, "cp_res");
  EXPECT_EQ("line1\nline2\n", cp_res);

  string foo = GetEntryContents(out_path, "foo");
  EXPECT_EQ("resline1\nresline2\n", foo);
}

// Extra combiners
TEST_F(OutputJarSimpleTest, ExtraCombiners) {
  string out_path = OutputFilePath("out.jar");
  const char kEntry[] = "tools/singlejar/data/extra_file1";
  output_jar_.ExtraCombiner(kEntry, new Concatenator(kEntry));
  CreateOutput(out_path, "--sources",
               DATA_DIR_TOP "src/tools/singlejar/libdata1.jar",
               DATA_DIR_TOP "src/tools/singlejar/libdata2.jar", nullptr);
  string extra_file_contents = GetEntryContents(out_path, kEntry);
  EXPECT_EQ(
      "extra_file_1 line1\n"
      "extra_file_1 line2\n"
      "extra_file_1 line1\n"
      "extra_file_1 line2\n",
      extra_file_contents);
}

// --include_headers
TEST_F(OutputJarSimpleTest, IncludeHeaders) {
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path, "--sources",
               DATA_DIR_TOP "src/tools/singlejar/libtest1.jar",
               DATA_DIR_TOP "src/tools/singlejar/libdata1.jar",
               "--include_prefixes", "tools/singlejar/data",
               nullptr);
  std::vector<string> expected_entries(
      {"META-INF/", "META-INF/MANIFEST.MF", "build-data.properties",
       "tools/singlejar/data/", "tools/singlejar/data/extra_file1",
       "tools/singlejar/data/extra_file2"});
  std::vector<string> jar_entries;
  InputJar input_jar;
  ASSERT_TRUE(input_jar.Open(out_path));
  const LH *lh;
  const CDH *cdh;
  while ((cdh = input_jar.NextEntry(&lh))) {
    jar_entries.push_back(cdh->file_name_string());
  }
  input_jar.Close();
  EXPECT_EQ(expected_entries, jar_entries);
}

// --normalize
TEST_F(OutputJarSimpleTest, Normalize) {
  // Creates output jar containing entries from all possible sources:
  //  * archives created by java_library rule, by jar tool, by zip
  //  * resource files
  //  * classpath resource files
  //  *
  string out_path = OutputFilePath("out.jar");
  string testjar_path = OutputFilePath("testinput.jar");
  {
    char *jar_tool_path = realpath(JAR_TOOL_PATH, nullptr);
    string textfile_path = CreateTextFile("jar_testinput.txt", "jar_inputtext");
    string classfile_path = CreateTextFile("JarTestInput.class", "Dummy");
    unlink(testjar_path.c_str());
    ASSERT_EQ(
        0, RunCommand(jar_tool_path, "-cf", testjar_path.c_str(),
                      textfile_path.c_str(), classfile_path.c_str(), nullptr));
    free(jar_tool_path);
  }

  string testzip_path = OutputFilePath("testinput.zip");
  {
    string textfile_path = CreateTextFile("zip_testinput.txt", "zip_inputtext");
    string classfile_path = CreateTextFile("ZipTestInput.class", "Dummy");
    unlink(testzip_path.c_str());
    ASSERT_EQ(
        0, RunCommand("zip", "-m", testzip_path.c_str(), textfile_path.c_str(),
                      classfile_path.c_str(), nullptr));
  }

  string resource_path = CreateTextFile("resource", "resource_text");
  string cp_resource_path = CreateTextFile("cp_resource", "cp_resource_text");

  // TODO(asmundak): check the following generated entries, too:
  //  * services
  //  * spring.schemas
  //  * spring.handlers
  //  * protobuf.meta
  //  * extra combiner

  CreateOutput(out_path, "--normalize", "--sources",
               DATA_DIR_TOP "src/tools/singlejar/libtest1.jar",
               testjar_path.c_str(), testzip_path.c_str(), "--resources",
               resource_path.c_str(), "--classpath_resources",
               cp_resource_path.c_str(), nullptr);

  // Scan all entries, verify that *.class entries have timestamp
  // 01/01/1980 00:00:02 and the rest have the timestamp of 01/01/1980 00:00:00.
  InputJar input_jar;
  ASSERT_TRUE(input_jar.Open(out_path));
  const LH *lh;
  const CDH *cdh;
  while ((cdh = input_jar.NextEntry(&lh))) {
    string entry_name = cdh->file_name_string();
    EXPECT_EQ(lh->last_mod_file_date(), cdh->last_mod_file_date())
        << entry_name << " modification date";
    EXPECT_EQ(lh->last_mod_file_time(), cdh->last_mod_file_time())
        << entry_name << " modification time";
    EXPECT_EQ(33, cdh->last_mod_file_date())
        << entry_name << " modification date should be 01/01/1980";
    auto n = entry_name.size() - strlen(".class");
    if (0 == strcmp(entry_name.c_str() + n, ".class")) {
      EXPECT_EQ(1, cdh->last_mod_file_time())
          << entry_name
          << " modification time for .class entry should be 00:00:02";
    } else {
      EXPECT_EQ(0, cdh->last_mod_file_time())
          << entry_name
          << " modification time for non .class entry should be 00:00:00";
    }
  }
  input_jar.Close();
}

// The files names META-INF/services/<something> are concatenated.
// The files named META-INF/spring.handlers are concatenated.
// The files named META-INF/spring.schemas are concatenated.
TEST_F(OutputJarSimpleTest, Services) {
  CreateTextFile("META-INF/services/spi.DateProvider",
                 "my.DateProviderImpl1\n");
  CreateTextFile("META-INF/services/spi.TimeProvider",
                 "my.TimeProviderImpl1\n");
  CreateTextFile("META-INF/spring.handlers", "handler1\n");
  CreateTextFile("META-INF/spring.schemas", "schema1\n");

  // We have to be in the output directory if we want to have entries in the
  // archive to start with META-INF. The resulting zip will contain 4 entries:
  //   META-INF/services/spi.DateProvider
  //   META-INF/services/spi.TimeProvider
  //   META-INF/spring.handlers
  //   META-INF/spring.schemas
  string out_dir = OutputFilePath("");
  ASSERT_EQ(0,
              RunCommand("cd", out_dir.c_str(), ";",
                         "zip", "-mr", "testinput1.zip", "META-INF", nullptr));
  string zip1_path = OutputFilePath("testinput1.zip");

  // Create the second zip, with 3 files:
  //   META-INF/services/spi.DateProvider.
  //   META-INF/spring.handlers
  //   META-INF/spring.schemas
  CreateTextFile("META-INF/services/spi.DateProvider",
                 "my.DateProviderImpl2\n");
  CreateTextFile("META-INF/spring.handlers", "handler2\n");
  CreateTextFile("META-INF/spring.schemas", "schema2\n");
  ASSERT_EQ(0,
              RunCommand("cd ", out_dir.c_str(), ";",
                         "zip", "-mr", "testinput2.zip", "META-INF", nullptr));
  string zip2_path = OutputFilePath("testinput2.zip");

  // The output jar should contain two service entries. The contents of the
  // META-INF/services/spi.DateProvider should be the concatenation of the
  // contents of this entry from both archives. And it should also contain
  // spring.handlers and spring.schemas entries.
  string out_path = OutputFilePath("out.jar");
  CreateOutput(out_path,
               "--sources", zip1_path.c_str(), zip2_path.c_str(), nullptr);
  EXPECT_EQ("my.DateProviderImpl1\n" "my.DateProviderImpl2\n",
            GetEntryContents(out_path, "META-INF/services/spi.DateProvider"));
  EXPECT_EQ("my.TimeProviderImpl1\n",
            GetEntryContents(out_path, "META-INF/services/spi.TimeProvider"));

  EXPECT_EQ("schema1\n" "schema2\n",
            GetEntryContents(out_path, "META-INF/spring.schemas"));
  EXPECT_EQ("handler1\n" "handler2\n",
            GetEntryContents(out_path, "META-INF/spring.handlers"));
}

}  // namespace