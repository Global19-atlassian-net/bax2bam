// Author: Derek Barnett

#include "TestData.h"
#include "TestUtils.h"

#include "HDFBasReader.hpp"
#include "utils/RegionUtils.hpp"
#include "HDFRegionTableReader.hpp"
#include <gtest/gtest.h>
#include <pbbam/BamFile.h>
#include <pbbam/BamRecord.h>
#include <pbbam/EntireFileQuery.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

using namespace PacBio;
using namespace PacBio::BAM;

// TODO: much of this is copypasta from src/SubreadConverter.cpp
struct SubreadInterval
{
    size_t Start;
    size_t End;
    PacBio::BAM::LocalContextFlags LocalContextFlags;

    SubreadInterval()
        : Start{0}
        , End{0}
        , LocalContextFlags{NO_LOCAL_CONTEXT}
    { }

    SubreadInterval(size_t start, size_t end, bool adapterBefore = false, bool adapterAfter = false)
        : Start{start}
        , End{end}
        , LocalContextFlags{(adapterBefore ? ADAPTER_BEFORE : NO_LOCAL_CONTEXT) |
                            (adapterAfter  ? ADAPTER_AFTER  : NO_LOCAL_CONTEXT)}
    { }
};

inline
bool RegionComparer(const RegionAnnotation& lhs, const RegionAnnotation& rhs)
{
    constexpr int HoleNumber  = RegionAnnotation::HOLENUMBERCOL;
    constexpr int RegionType  = RegionAnnotation::REGIONTYPEINDEXCOL;
    constexpr int RegionStart = RegionAnnotation::REGIONSTARTCOL;

    if (lhs.row[HoleNumber] < rhs.row[HoleNumber])
        return true;
    else if (lhs.row[HoleNumber] == rhs.row[HoleNumber])
    {
        if (lhs.row[RegionType] < rhs.row[RegionType])
            return true;
        else if (lhs.row[RegionType] == rhs.row[RegionType])
            return lhs.row[RegionStart] < rhs.row[RegionStart];
    }
    return false;
}

void ComputeSubreadIntervals(std::vector<SubreadInterval>* const intervals,
                             RegionTable& regionTable,
                             const unsigned holeNumber)
{
    constexpr int RegionStart = RegionAnnotation::REGIONSTARTCOL;
    constexpr int RegionEnd   = RegionAnnotation::REGIONENDCOL;

    // clear the input first
    intervals->clear();

    RegionAnnotations zmwRegions = regionTable[holeNumber];

    // Has non-empty HQRegion or not?
    if (not zmwRegions.HasHQRegion())
        return;
        //throw std::runtime_error("could not find HQRegion for ZMW " + std::to_string(holeNumber));

    size_t hqStart = zmwRegions.HQStart();
    size_t hqEnd   = zmwRegions.HQEnd();

    if (hqEnd <= hqStart)
        return;

    // this logic mirrors that in the C# codebase for DelimitedSeqRegions rather
    // than what's in src/SubreadConverter.cpp for verification purposes
    ReadInterval const * lastAdapter = nullptr;
    bool prevIsAdapter = false;
    size_t regStart = hqStart;
    std::vector<ReadInterval> adapters = zmwRegions.AdapterIntervals();
    for (size_t i = 0; i < adapters.size(); i++) { 
        ReadInterval adapter = adapters[i];
        size_t adapterStart = adapter.start;
        size_t adapterEnd   = adapter.end;

        if (adapterEnd < hqStart)
            continue;

        if (adapterStart > hqEnd)
            break;

        if (prevIsAdapter)
            intervals->emplace_back(SubreadInterval(lastAdapter->end, adapterStart, true, true));
        else if (regStart < adapterStart)
            intervals->emplace_back(SubreadInterval(regStart, adapterStart, false, true));

        lastAdapter = &adapters[i];
        prevIsAdapter = true;
        regStart = adapterEnd;
    }

    if (prevIsAdapter)
        intervals->emplace_back(SubreadInterval(lastAdapter->end, hqEnd, true, false));
    else if (regStart < hqEnd)
        intervals->emplace_back(SubreadInterval(regStart, hqEnd, false, false));
}


TEST(SubreadsTest, EndToEnd_Multiple)
{
    // setup
    const std::string movieName = "m160823_221224_ethan_c010091942559900001800000112311890_s1_p0";

    std::vector<std::string> baxFilenames;
    baxFilenames.push_back(tests::Data_Dir + "/" + movieName + ".1.bax.h5");

    const std::string generatedBam = movieName + ".subreads.bam";
    const std::string scrapBam = movieName + ".scraps.bam";

    // run conversion
    const int result = RunBax2Bam(baxFilenames, "--subread");
    EXPECT_EQ(0, result);

    {   // ensure PBIs exist
        const BamFile generatedBamFile(generatedBam);
        const BamFile scrapsBamFile(scrapBam);
        EXPECT_TRUE(generatedBamFile.PacBioIndexExists());
        EXPECT_TRUE(scrapsBamFile.PacBioIndexExists());
    }

    // open BAX reader on original data
    HDFBasReader baxReader;
    baxReader.IncludeField("Basecall");
    baxReader.IncludeField("DeletionQV");
    baxReader.IncludeField("DeletionTag");
    baxReader.IncludeField("InsertionQV");
    baxReader.IncludeField("PreBaseFrames");
    baxReader.IncludeField("MergeQV");
    baxReader.IncludeField("SubstitutionQV");
    baxReader.IncludeField("HQRegionSNR");
    baxReader.IncludeField("WidthInFrames");
    // not using SubTag here

    std::string baxBasecallerVersion;
    std::string baxBindingKit;
    std::string baxSequencingKit;

    const int initOk = baxReader.Initialize(baxFilenames.front());
    EXPECT_EQ(1, initOk);
    if (initOk == 1) {

        if (baxReader.scanDataReader.fileHasScanData && baxReader.scanDataReader.initializedRunInfoGroup) {

            if (baxReader.scanDataReader.runInfoGroup.ContainsAttribute("BindingKit")) {
                HDFAtom<std::string> bkAtom;
                if (bkAtom.Initialize(baxReader.scanDataReader.runInfoGroup, "BindingKit")) {
                    bkAtom.Read(baxBindingKit);
                    bkAtom.dataspace.close();
                }
            }

            if (baxReader.scanDataReader.runInfoGroup.ContainsAttribute("SequencingKit")) {
                HDFAtom<std::string> skAtom;
                if (skAtom.Initialize(baxReader.scanDataReader.runInfoGroup, "SequencingKit")) {
                    skAtom.Read(baxSequencingKit);
                    skAtom.dataspace.close();
                }
            }
        }

        baxReader.GetChangeListID(baxBasecallerVersion);
    }

    // read region table info
    std::unique_ptr<HDFRegionTableReader> const regionTableReader(new HDFRegionTableReader);
    RegionTable regionTable;
    std::string fn = baxFilenames.front();
    EXPECT_TRUE(regionTableReader->Initialize(fn) != 0);
    regionTable.Reset();
    regionTableReader->ReadTable(regionTable);
    regionTableReader->Close();

    EXPECT_NO_THROW(
    {
        // open BAM file
        BamFile bamFile(generatedBam);

        // check BAM header information
        const BamHeader& header = bamFile.Header();
        EXPECT_EQ(tests::Header_Version,     header.Version());
        EXPECT_EQ(std::string("unknown"), header.SortOrder());
        EXPECT_EQ(tests::PacBioBam_Version,  header.PacBioBamVersion());
        EXPECT_TRUE(header.Sequences().empty());
        EXPECT_TRUE(header.Comments().empty());
        ASSERT_FALSE(header.Programs().empty());

        const std::vector<std::string> readGroupIds = header.ReadGroupIds();
        ASSERT_FALSE(readGroupIds.empty());
        const ReadGroupInfo& rg = header.ReadGroup(readGroupIds.front());

        std::string rawId = movieName + "//SUBREAD";
        std::string md5Id;
        MakeMD5(rawId, md5Id, 8);
        EXPECT_EQ(md5Id, rg.Id());

        EXPECT_EQ(std::string("PACBIO"), rg.Platform());
        EXPECT_EQ(movieName, rg.MovieName());

        EXPECT_TRUE(rg.SequencingCenter().empty());
        EXPECT_TRUE(rg.Date().empty());
        EXPECT_TRUE(rg.FlowOrder().empty());
        EXPECT_TRUE(rg.KeySequence().empty());
        EXPECT_TRUE(rg.Library().empty());
        EXPECT_TRUE(rg.Programs().empty());
        EXPECT_TRUE(rg.PredictedInsertSize().empty());
        EXPECT_TRUE(rg.Sample().empty());

        EXPECT_EQ("SUBREAD", rg.ReadType());
        EXPECT_EQ(baxBasecallerVersion, rg.BasecallerVersion());
        EXPECT_EQ(baxBindingKit, rg.BindingKit());
        EXPECT_EQ(baxSequencingKit, rg.SequencingKit());
        EXPECT_FLOAT_EQ(75.00577, std::stof(rg.FrameRateHz()));
        EXPECT_EQ("dq", rg.BaseFeatureTag(BaseFeature::DELETION_QV));
        EXPECT_EQ("dt", rg.BaseFeatureTag(BaseFeature::DELETION_TAG));
        EXPECT_EQ("iq", rg.BaseFeatureTag(BaseFeature::INSERTION_QV));
        EXPECT_EQ("ip", rg.BaseFeatureTag(BaseFeature::IPD));
        EXPECT_EQ("mq", rg.BaseFeatureTag(BaseFeature::MERGE_QV));
        EXPECT_EQ("sq", rg.BaseFeatureTag(BaseFeature::SUBSTITUTION_QV));
        EXPECT_EQ("pw", rg.BaseFeatureTag(BaseFeature::PULSE_WIDTH));
        EXPECT_FALSE(rg.HasBaseFeature(BaseFeature::SUBSTITUTION_TAG));
        EXPECT_EQ(FrameCodec::V1, rg.IpdCodec());

        // compare 1st record from each file
        SMRTSequence baxRecord;
        auto holeNumber = 0;
        std::vector<float> hqSnr;

        size_t intervalIdx = 0;
        std::vector<SubreadInterval> subreadIntervals;

        size_t numTested = 0;
        EntireFileQuery entireFile(bamFile);
        for (BamRecord& bamRecord : entireFile) {
 
            if (numTested > 30)
                goto cleanup;

            if (intervalIdx >= subreadIntervals.size())
            {
                while (baxReader.GetNext(baxRecord))
                {
                    holeNumber  = baxRecord.zmwData.holeNumber;

                    ComputeSubreadIntervals(&subreadIntervals, regionTable, holeNumber);

                    /* this is for debugging subread interval problems
                    int hqStart = 0;
                    int hqEnd = 0;
                    int hqScore = 0;
                    LookupHQRegion(holeNumber,
                                   regionTable,
                                   hqStart,
                                   hqEnd,
                                   hqScore);

                    std::vector<ReadInterval> subreadIntervals_;
                    CollectSubreadIntervals(baxRecord, &regionTable, subreadIntervals_);

                    for (int i = subreadIntervals_.size() - 1; i >= 0; --i)
                    {
                        auto& in = subreadIntervals_[i];
                        int inStart = std::max(hqStart, in.start);
                        int inEnd   = std::min(hqEnd,   in.end);
                        if (inEnd <= inStart)
                            subreadIntervals_.erase(subreadIntervals_.begin() + i);
                    }

                    std::cerr << "hqRegion: " << hqStart << ", " << hqEnd << std::endl;
                    std::cerr << "subreadRegions:" << std::endl;
                    for (const auto& in : subreadIntervals_)
                        std::cerr << "  l, r: " << in.start << ", " << in.end << std::endl;

                    std::cerr << "adapterDerived:" << std::endl;
                    for (const auto& in : subreadIntervals)
                        std::cerr << "  l, r: " << in.Start << ", " << in.End << std::endl;

                    std::cerr << std::endl;
                    // */

                    if (subreadIntervals.empty())
                        continue;

                    intervalIdx = 0;

                    hqSnr.clear();
                    hqSnr.push_back(baxRecord.HQRegionSnr('A'));
                    hqSnr.push_back(baxRecord.HQRegionSnr('C'));
                    hqSnr.push_back(baxRecord.HQRegionSnr('G'));
                    hqSnr.push_back(baxRecord.HQRegionSnr('T'));

                    EXPECT_GT(hqSnr[0], 0);
                    EXPECT_GT(hqSnr[1], 0);
                    EXPECT_GT(hqSnr[2], 0);
                    EXPECT_GT(hqSnr[3], 0);

                    goto compare;
                }

                goto cleanup;
            }

compare:
            const BamRecordImpl& bamRecordImpl = bamRecord.Impl();
            EXPECT_EQ(4680U,bamRecordImpl.Bin());
            EXPECT_EQ(0,   bamRecordImpl.InsertSize());
            EXPECT_EQ(255, bamRecordImpl.MapQuality());
            EXPECT_EQ(-1,  bamRecordImpl.MatePosition());
            EXPECT_EQ(-1,  bamRecordImpl.MateReferenceId());
            EXPECT_EQ(-1,  bamRecordImpl.Position());
            EXPECT_EQ(-1,  bamRecordImpl.ReferenceId());
            EXPECT_FALSE(bamRecordImpl.IsMapped());

            const int subreadStart = subreadIntervals[intervalIdx].Start;
            const int subreadEnd   = subreadIntervals[intervalIdx].End;

            const std::string expectedName = movieName + "/" +
                    std::to_string(holeNumber)   + "/" +
                    std::to_string(subreadStart) + "_" +
                    std::to_string(subreadEnd);
            EXPECT_EQ(expectedName, bamRecordImpl.Name());

            using PacBio::BAM::QualityValue;
            using PacBio::BAM::QualityValues;

            const DNALength length = subreadEnd - subreadStart;

            std::string expectedSequence;
            expectedSequence.assign((const char*)baxRecord.seq + subreadStart, length);

            const std::string bamSequence = bamRecord.Sequence();
            const QualityValues bamQualities = bamRecord.Qualities();
            EXPECT_EQ(expectedSequence, bamSequence);
            EXPECT_TRUE(bamQualities.empty());

            const QualityValues bamDeletionQVs = bamRecord.DeletionQV();
            const QualityValues bamInsertionQVs = bamRecord.InsertionQV();
            const QualityValues bamMergeQVs = bamRecord.MergeQV();
            const QualityValues bamSubstitutionQVs = bamRecord.SubstitutionQV();

            for (size_t i = 0; i < length; ++i) {
                const size_t pos = subreadStart + i;

                EXPECT_EQ((QualityValue)baxRecord.GetDeletionQV(pos),     bamDeletionQVs.at(i));
                EXPECT_EQ((QualityValue)baxRecord.GetInsertionQV(pos),    bamInsertionQVs.at(i));
                EXPECT_EQ((QualityValue)baxRecord.GetMergeQV(pos),        bamMergeQVs.at(i));
                EXPECT_EQ((QualityValue)baxRecord.GetSubstitutionQV(pos), bamSubstitutionQVs.at(i));
            }

            if (baxRecord.deletionTag)
            {
                std::string expectedDeletionTags;
                expectedDeletionTags.assign((char*)baxRecord.deletionTag + subreadStart,
                                            (char*)baxRecord.deletionTag + subreadStart + length);
                const std::string& bamDeletionTags = bamRecord.DeletionTag();
                EXPECT_EQ(expectedDeletionTags, bamDeletionTags);
            }

            if (baxRecord.substitutionTag)
            {
                std::string expectedSubstitutionTags;
                expectedSubstitutionTags.assign((char*)baxRecord.substitutionTag + subreadStart,
                                            (char*)baxRecord.substitutionTag + subreadStart + length);
                const std::string& bamSubstitutionTags = bamRecord.SubstitutionTag();
                EXPECT_EQ(expectedSubstitutionTags, bamSubstitutionTags);
            }

            // TODO: IPDs
            const LocalContextFlags ctxFlags = subreadIntervals[intervalIdx].LocalContextFlags;

            EXPECT_EQ(md5Id,        bamRecord.ReadGroupId());
            EXPECT_EQ(movieName,    bamRecord.MovieName());
            EXPECT_EQ(1,            bamRecord.NumPasses());
            EXPECT_EQ(holeNumber,   bamRecord.HoleNumber());
            EXPECT_EQ(subreadStart, bamRecord.QueryStart());
            EXPECT_EQ(subreadEnd,   bamRecord.QueryEnd());
            EXPECT_EQ(hqSnr,        bamRecord.SignalToNoise());
            EXPECT_EQ(ctxFlags,     bamRecord.LocalContextFlags());

            numTested++;
            intervalIdx++;
        }

cleanup:
        EXPECT_GT(numTested, 1UL);

        // cleanup
        baxReader.Close();
        RemoveFile(generatedBam);
        RemoveFile(scrapBam);
        RemoveFile(generatedBam + ".pbi");
        RemoveFile(scrapBam + ".pbi");

    }); // EXPECT_NO_THROW
}
