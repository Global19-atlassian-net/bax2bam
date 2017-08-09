// Author: Derek Barnett

#include "TestData.h"
#include "TestUtils.h"

#include "HDFBasReader.hpp"
#include <gtest/gtest.h>
#include <pbbam/BamFile.h>
#include <pbbam/BamRecord.h>
#include <pbbam/EntireFileQuery.h>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

using namespace PacBio;
using namespace PacBio::BAM;

TEST(PolymeraseTest, EndToEnd_Single)
{
    // setup
    const std::string movieName = "m160823_221224_ethan_c010091942559900001800000112311890_s1_p0";

    std::vector<std::string> baxFilenames;
    baxFilenames.push_back(tests::Data_Dir + "/" + movieName + ".1.bax.h5");

    const std::string generatedBam = movieName + ".polymerase.bam";

    // run conversion
    const int result = RunBax2Bam(baxFilenames, "--polymeraseread");
    EXPECT_EQ(0, result);

    {   // ensure PBI exists
        const BamFile generatedBamFile(generatedBam);
        EXPECT_TRUE(generatedBamFile.PacBioIndexExists());
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
    // not using SubTag

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

        std::string rawId = movieName + "//POLYMERASE";
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

        EXPECT_EQ("POLYMERASE", rg.ReadType());
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
        EXPECT_TRUE(baxReader.GetReadAt(8, baxRecord) > 0);

        std::vector<float> hqSnr;
        hqSnr.push_back(baxRecord.HQRegionSnr('A'));
        hqSnr.push_back(baxRecord.HQRegionSnr('C'));
        hqSnr.push_back(baxRecord.HQRegionSnr('G'));
        hqSnr.push_back(baxRecord.HQRegionSnr('T'));

        EXPECT_FLOAT_EQ(0.0, hqSnr[0]);
        EXPECT_FLOAT_EQ(0.0, hqSnr[1]);
        EXPECT_FLOAT_EQ(0.0, hqSnr[2]);
        EXPECT_FLOAT_EQ(0.0, hqSnr[3]);

        bool firstRecord = true;
        EntireFileQuery entireFile(bamFile);
        for ( BamRecord& bamRecord : entireFile) {
            if (!firstRecord)
                break;
            firstRecord = false;

            const BamRecordImpl& bamRecordImpl = bamRecord.Impl();
            EXPECT_EQ(4680U,bamRecordImpl.Bin());
            EXPECT_EQ(0,   bamRecordImpl.InsertSize());
            EXPECT_EQ(255, bamRecordImpl.MapQuality());
            EXPECT_EQ(-1,  bamRecordImpl.MatePosition());
            EXPECT_EQ(-1,  bamRecordImpl.MateReferenceId());
            EXPECT_EQ(-1,  bamRecordImpl.Position());
            EXPECT_EQ(-1,  bamRecordImpl.ReferenceId());
            EXPECT_FALSE(bamRecordImpl.IsMapped());

            const int holeNumber    = baxRecord.zmwData.holeNumber;
            const int subreadStart  = 0;
            const int subreadEnd    = baxRecord.length;

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

            EXPECT_EQ(md5Id,        bamRecord.ReadGroupId());
            EXPECT_EQ(movieName,    bamRecord.MovieName());
            EXPECT_EQ(1,            bamRecord.NumPasses());
            EXPECT_EQ(holeNumber,   bamRecord.HoleNumber());
            EXPECT_EQ(subreadStart, bamRecord.QueryStart());
            EXPECT_EQ(subreadEnd,   bamRecord.QueryEnd());
            EXPECT_EQ(hqSnr,        bamRecord.SignalToNoise());
            EXPECT_FALSE(bamRecord.HasLocalContextFlags());
        }

        // cleanup
        baxReader.Close();
        RemoveFile(generatedBam);
        RemoveFile(generatedBam + ".pbi");

    }); // EXPECT_NO_THROW
}
