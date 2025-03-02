// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/metrics/structured/external_metrics.h"
#include "components/metrics/structured/structured_metrics_features.h"

#include <memory>
#include <numeric>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "components/metrics/structured/storage.pb.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace metrics {
namespace structured {
namespace {

using testing::UnorderedElementsAre;

// Make a simple testing proto with one |uma_events| message for each id in
// |ids|.
EventsProto MakeTestingProto(const std::vector<uint64_t>& ids,
                             uint64_t project_name_hash = 0) {
  EventsProto proto;

  for (const auto id : ids) {
    auto* event = proto.add_uma_events();
    event->set_project_name_hash(project_name_hash);
    event->set_profile_event_id(id);
  }

  return proto;
}

// Check that |proto| is consistent with the proto that would be generated by
// MakeTestingProto(ids).
void AssertEqualsTestingProto(const EventsProto& proto,
                              const std::vector<uint64_t>& ids) {
  ASSERT_EQ(proto.uma_events().size(), static_cast<int>(ids.size()));
  ASSERT_TRUE(proto.non_uma_events().empty());

  for (size_t i = 0; i < ids.size(); ++i) {
    const auto& event = proto.uma_events(i);
    ASSERT_EQ(event.profile_event_id(), ids[i]);
    ASSERT_FALSE(event.has_event_name_hash());
    ASSERT_TRUE(event.metrics().empty());
  }
}

}  // namespace

class ExternalMetricsTest : public testing::Test {
 public:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());

    // TODO(b/181724341): Remove this when the bluetooth metrics feature is
    // enabled by default.
    scoped_feature_list_.InitWithFeatures(
        /*enabled_features=*/{},
        /*disabled_features=*/{kBluetoothSessionizedMetrics});
  }

  void Init() {
    // We don't use the scheduling feature when testing ExternalMetrics, instead
    // we just call CollectMetrics directly. So make up a time interval here
    // that we'll never reach in a test.
    const auto one_hour = base::Hours(1);
    external_metrics_ = std::make_unique<ExternalMetrics>(
        temp_dir_.GetPath(), one_hour,
        base::BindRepeating(&ExternalMetricsTest::OnEventsCollected,
                            base::Unretained(this)));

    // For most tests the recording needs to be enabled.
    EnableRecording();
  }

  void EnableRecording() { external_metrics_->EnableRecording(); }

  void DisableRecording() { external_metrics_->DisableRecording(); }

  void CollectEvents() {
    external_metrics_->CollectEvents();
    Wait();
    CHECK(proto_.has_value());
  }

  void OnEventsCollected(const EventsProto& proto) {
    proto_ = std::move(proto);
  }

  void WriteToDisk(const std::string& name, const EventsProto& proto) {
    CHECK(base::WriteFile(temp_dir_.GetPath().Append(name),
                          proto.SerializeAsString()));
  }

  void WriteToDisk(const std::string& name, const std::string& str) {
    CHECK(base::WriteFile(temp_dir_.GetPath().Append(name), str));
  }

  void Wait() { task_environment_.RunUntilIdle(); }

  base::test::ScopedFeatureList scoped_feature_list_;
  base::ScopedTempDir temp_dir_;
  std::unique_ptr<ExternalMetrics> external_metrics_;
  absl::optional<EventsProto> proto_;

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::UI,
      base::test::TaskEnvironment::ThreadPoolExecutionMode::QUEUED};
  base::HistogramTester histogram_tester_;
};

TEST_F(ExternalMetricsTest, ReadOneFile) {
  // Make one proto with three events.
  WriteToDisk("myproto", MakeTestingProto({111, 222, 333}));
  Init();

  CollectEvents();

  // We should have correctly picked up the three events.
  AssertEqualsTestingProto(proto_.value(), {111, 222, 333});
  // And the directory should now be empty.
  ASSERT_TRUE(base::IsDirectoryEmpty(temp_dir_.GetPath()));
}

TEST_F(ExternalMetricsTest, ReadManyFiles) {
  // Make three protos with three events each.
  WriteToDisk("first", MakeTestingProto({111, 222, 333}));
  WriteToDisk("second", MakeTestingProto({444, 555, 666}));
  WriteToDisk("third", MakeTestingProto({777, 888, 999}));
  Init();

  CollectEvents();

  // We should have correctly picked up the nine events. Don't check for order,
  // because we can't guarantee the files will be read from disk in any
  // particular order.
  std::vector<int64_t> ids;
  for (const auto& event : proto_.value().uma_events()) {
    ids.push_back(event.profile_event_id());
  }
  ASSERT_THAT(
      ids, UnorderedElementsAre(111, 222, 333, 444, 555, 666, 777, 888, 999));

  // The directory should be empty after reading.
  ASSERT_TRUE(base::IsDirectoryEmpty(temp_dir_.GetPath()));
}

TEST_F(ExternalMetricsTest, ReadZeroFiles) {
  Init();
  CollectEvents();
  // We should have an empty proto.
  AssertEqualsTestingProto(proto_.value(), {});
  // And the directory should be empty too.
  ASSERT_TRUE(base::IsDirectoryEmpty(temp_dir_.GetPath()));
}

TEST_F(ExternalMetricsTest, CollectTwice) {
  Init();
  WriteToDisk("first", MakeTestingProto({111, 222, 333}));
  CollectEvents();
  AssertEqualsTestingProto(proto_.value(), {111, 222, 333});

  WriteToDisk("first", MakeTestingProto({444}));
  CollectEvents();
  AssertEqualsTestingProto(proto_.value(), {444});
}

TEST_F(ExternalMetricsTest, HandleCorruptFile) {
  Init();

  WriteToDisk("invalid", "surprise i'm not a proto");
  WriteToDisk("valid", MakeTestingProto({111, 222, 333}));

  CollectEvents();
  AssertEqualsTestingProto(proto_.value(), {111, 222, 333});
  // Should have deleted the invalid file too.
  ASSERT_TRUE(base::IsDirectoryEmpty(temp_dir_.GetPath()));
}

// TODO(b/181724341): Remove this when the bluetooth metrics feature is enabled
// by default.
TEST_F(ExternalMetricsTest, FilterBluetoothEvents) {
  // Event name hash for cros's BluetoothPairingStateChanged event.
  const uint64_t event_hash = UINT64_C(11839023048095184048);

  Init();

  // Use the profile_event_id as an marker of which event is which, and assign a
  // bluetooth event hash to ids > 100.
  EventsProto proto;
  for (const auto id : {101, 1, 2, 102, 103, 3, 104}) {
    auto* event = proto.add_uma_events();
    event->set_profile_event_id(id);
    if (id > 100) {
      event->set_event_name_hash(event_hash);
    }
  }
  WriteToDisk("proto", proto);

  CollectEvents();
  AssertEqualsTestingProto(proto_.value(), {1, 2, 3});
}

TEST_F(ExternalMetricsTest, FileNumberReadCappedAndDiscarded) {
  // Setup feature.
  base::test::ScopedFeatureList feature_list;
  const int file_limit = 2;
  feature_list.InitAndEnableFeatureWithParameters(
      features::kStructuredMetrics,
      {{"file_limit", base::NumberToString(file_limit)}});

  Init();

  // File limit is set to 2. Include third file to test that it is omitted and
  // deleted.
  WriteToDisk("first", MakeTestingProto({111}));
  WriteToDisk("second", MakeTestingProto({222}));
  WriteToDisk("third", MakeTestingProto({333}));

  CollectEvents();

  // Number of events should be capped to the file limit since above records one
  // event per file.
  ASSERT_EQ(proto_.value().uma_events().size(), file_limit);

  // And the directory should be empty too.
  ASSERT_TRUE(base::IsDirectoryEmpty(temp_dir_.GetPath()));
}

TEST_F(ExternalMetricsTest, FilterDisallowedProjects) {
  Init();
  external_metrics_->AddDisallowedProjectForTest(2);

  // Add 3 events with a project of 1 and 2.
  WriteToDisk("first", MakeTestingProto({111}, 1));
  WriteToDisk("second", MakeTestingProto({222}, 2));
  WriteToDisk("third", MakeTestingProto({333}, 1));

  CollectEvents();

  // The events at second should be filtered.
  ASSERT_EQ(proto_.value().uma_events().size(), 2);

  std::vector<int64_t> ids;
  for (const auto& event : proto_.value().uma_events()) {
    ids.push_back(event.profile_event_id());
  }

  // Validate that only project 1 remains.
  ASSERT_THAT(ids, UnorderedElementsAre(111, 333));

  // And the directory should be empty too.
  ASSERT_TRUE(base::IsDirectoryEmpty(temp_dir_.GetPath()));
}

TEST_F(ExternalMetricsTest, DroppedEventsWhenDisabled) {
  Init();
  DisableRecording();

  // Add 3 events with a project of 1 and 2.
  WriteToDisk("first", MakeTestingProto({111}, 1));
  WriteToDisk("second", MakeTestingProto({222}, 2));
  WriteToDisk("third", MakeTestingProto({333}, 1));

  CollectEvents();

  // No events should have been collected.
  ASSERT_EQ(proto_.value().uma_events().size(), 0);

  // And the directory should be empty too.
  ASSERT_TRUE(base::IsDirectoryEmpty(temp_dir_.GetPath()));
}

TEST_F(ExternalMetricsTest, ProducedAndDroppedEventMetricCollected) {
  base::test::ScopedFeatureList feature_list;
  const int file_limit = 5;
  feature_list.InitAndEnableFeatureWithParameters(
      features::kStructuredMetrics,
      {{"file_limit", base::NumberToString(file_limit)}});

  Init();

  // Wifi
  WriteToDisk("event1", MakeTestingProto({0}, UINT64_C(4320592646346933548)));
  WriteToDisk("event2", MakeTestingProto({1}, UINT64_C(4320592646346933548)));
  // Bluetooth
  WriteToDisk("event3", MakeTestingProto({2}, UINT64_C(9074739597929991885)));
  WriteToDisk("event4", MakeTestingProto({3}, UINT64_C(9074739597929991885)));
  // Cellular
  WriteToDisk("event5", MakeTestingProto({4}, UINT64_C(8206859287963243715)));
  WriteToDisk("event6", MakeTestingProto({5}, UINT64_C(8206859287963243715)));
  // WIfi
  WriteToDisk("event7", MakeTestingProto({6}, UINT64_C(4320592646346933548)));
  WriteToDisk("event8", MakeTestingProto({7}, UINT64_C(4320592646346933548)));
  // Bluetooth
  WriteToDisk("event9", MakeTestingProto({8}, UINT64_C(9074739597929991885)));
  WriteToDisk("event10", MakeTestingProto({9}, UINT64_C(9074739597929991885)));

  CollectEvents();

  ASSERT_EQ(proto_.value().uma_events().size(), file_limit);

  // Unable to guarantee the order the events are read in. Using counts to
  // verify that the number of histograms produced are what is expected.
  base::HistogramTester::CountsMap produced_map =
      histogram_tester_.GetTotalCountsForPrefix(
          "StructuredMetrics.ExternalMetricsProduced.");
  int produced_acc = 0;
  for (const auto& hist : produced_map) {
    produced_acc += hist.second;
  }

  base::HistogramTester::CountsMap dropped_map =
      histogram_tester_.GetTotalCountsForPrefix(
          "StructuredMetrics.ExternalMetricsDropped.");

  int dropped_acc = 0;
  for (const auto& hist : dropped_map) {
    dropped_acc += hist.second;
  }

  EXPECT_EQ(produced_acc, 3);
  EXPECT_EQ(dropped_acc, 3);
}

// TODO(crbug.com/1148168): Add a test for concurrent reading and writing here
// once we know the specifics of how the lock in cros is performed.

}  // namespace structured
}  // namespace metrics
