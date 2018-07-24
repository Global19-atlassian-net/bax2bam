// Author: Derek Barnett

#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

#include <gtest/gtest.h>

#include <pbbam/BamFile.h>
#include <pbbam/BamRecord.h>
#include <pbbam/EntireFileQuery.h>

#include <pbdata/CCSSequence.hpp>
#include <hdf/HDFCCSReader.hpp>

#include "TestData.h"
#include "TestUtils.h"

using namespace PacBio;
using namespace PacBio::BAM;

TEST(CcsTest, EndToEnd_Multiple)
{
    // setup
    const std::string movieName = "m131018_081703_42161_c100585152550000001823088404281404_s1_p0";

    std::vector<std::string> baxFilenames;
    baxFilenames.push_back(tests::Data_Dir + "/data/" + movieName + ".1.ccs.h5");

    const std::string generatedBam = movieName + ".ccs.bam";

    // run conversion
    const int result = RunBax2Bam(baxFilenames, "--ccs");
    EXPECT_EQ(0, result);

    {   // ensure PBI exists
        const BamFile generatedBamFile(generatedBam);
        EXPECT_TRUE(generatedBamFile.PacBioIndexExists());
    }

    // open BAX reader on original data
    HDFCCSReader<CCSSequence> baxReader;
    baxReader.IncludeField("Basecall");
    baxReader.IncludeField("QualityValue");
    baxReader.IncludeField("DeletionQV");
    baxReader.IncludeField("InsertionQV");
    baxReader.IncludeField("SubstitutionQV");

    std::string baxBasecallerVersion;
    std::string baxBindingKit;
    std::string baxSequencingKit;

    // set magic bits
    baxReader.SetReadBasesFromCCS();

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

            {
                HDFGroup bcGroup;
                if (baxReader.pulseDataGroup.ContainsObject("BaseCalls") &&
                    bcGroup.Initialize(baxReader.pulseDataGroup.group, "BaseCalls"))
                {
                    HDFAtom<std::string> clAtom;
                    if (bcGroup.ContainsAttribute("ChangeListID") &&
                        clAtom.Initialize(bcGroup.group, "ChangeListID"))
                    {
                        clAtom.Read(baxBasecallerVersion);
                        clAtom.dataspace.close();
                    }
                    bcGroup.Close();
                }
            }
        }
    }

    EXPECT_NO_THROW(
    {
        // open BAM file
        BamFile bamFile(generatedBam);

        // check BAM header information
        const BamHeader& header = bamFile.Header();
        EXPECT_EQ(std::string("unknown"), header.SortOrder());
        EXPECT_EQ(tests::PacBioBam_Version,   header.PacBioBamVersion());
        EXPECT_TRUE(header.Sequences().empty());
        EXPECT_TRUE(header.Comments().empty());
        ASSERT_FALSE(header.Programs().empty());

        const std::vector<std::string> readGroupIds = header.ReadGroupIds();
        ASSERT_FALSE(readGroupIds.empty());
        const ReadGroupInfo& rg = header.ReadGroup(readGroupIds.front());

        std::string rawId = movieName + "//CCS";
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

        EXPECT_EQ("CCS", rg.ReadType());
        EXPECT_EQ(baxBasecallerVersion, rg.BasecallerVersion());
        EXPECT_EQ(baxBindingKit, rg.BindingKit());
        EXPECT_EQ(baxSequencingKit, rg.SequencingKit());
        EXPECT_EQ(75, std::stod(rg.FrameRateHz()));
        EXPECT_EQ("dq", rg.BaseFeatureTag(BaseFeature::DELETION_QV));
        EXPECT_EQ("iq", rg.BaseFeatureTag(BaseFeature::INSERTION_QV));
        EXPECT_EQ("sq", rg.BaseFeatureTag(BaseFeature::SUBSTITUTION_QV));
        EXPECT_FALSE(rg.HasBaseFeature(BaseFeature::DELETION_TAG));
        EXPECT_FALSE(rg.HasBaseFeature(BaseFeature::IPD));
        EXPECT_FALSE(rg.HasBaseFeature(BaseFeature::MERGE_QV));
        EXPECT_FALSE(rg.HasBaseFeature(BaseFeature::SUBSTITUTION_TAG));

        // compare 1st record from each file
        CCSSequence baxRecord;
        const UInt holeNumber = baxRecord.zmwData.holeNumber;

        size_t numTested = 0;
        EntireFileQuery entireFile(bamFile);
        for (BamRecord& bamRecord : entireFile) {
            while (baxReader.GetNext(baxRecord)) {
                if (baxRecord.length > 0)
                    goto compare;
            }
            goto cleanup;

compare:
            EXPECT_GT(baxRecord.length, 0U);
            const BamRecordImpl& bamRecordImpl = bamRecord.Impl();
            EXPECT_EQ(4680U,bamRecordImpl.Bin());
            EXPECT_EQ(0,   bamRecordImpl.InsertSize());
            EXPECT_EQ(255, bamRecordImpl.MapQuality());
            EXPECT_EQ(-1,  bamRecordImpl.MatePosition());
            EXPECT_EQ(-1,  bamRecordImpl.MateReferenceId());
            EXPECT_EQ(-1,  bamRecordImpl.Position());
            EXPECT_EQ(-1,  bamRecordImpl.ReferenceId());
            EXPECT_FALSE(bamRecordImpl.IsMapped());

            const int holeNumber      = baxRecord.zmwData.holeNumber;
            const int numPasses       = baxRecord.numPasses;
            const std::string expectedName = baxRecord.GetName();
            EXPECT_EQ(expectedName, bamRecordImpl.Name());

            using PacBio::BAM::QualityValue;
            using PacBio::BAM::QualityValues;

            const DNALength length = baxRecord.length;

            std::string expectedSequence;
            expectedSequence.assign((const char*)baxRecord.seq, length);

            QualityValues expectedQualities;
            expectedQualities.assign((uint8_t*)baxRecord.qual.data, baxRecord.qual.data + length);

            const std::string bamSequence = bamRecord.Sequence();
            const QualityValues bamQualities = bamRecord.Qualities();
            EXPECT_EQ(expectedSequence,  bamSequence);
            EXPECT_EQ(expectedQualities, bamQualities);

            const QualityValues bamDeletionQVs     = bamRecord.DeletionQV();
            const QualityValues bamInsertionQVs    = bamRecord.InsertionQV();
            const QualityValues bamSubstitutionQVs = bamRecord.SubstitutionQV();

            for (size_t i = 0; i < length; ++i) {
                EXPECT_EQ((QualityValue)baxRecord.GetDeletionQV(i),     bamDeletionQVs.at(i));
                EXPECT_EQ((QualityValue)baxRecord.GetInsertionQV(i),    bamInsertionQVs.at(i));
                EXPECT_EQ((QualityValue)baxRecord.GetSubstitutionQV(i), bamSubstitutionQVs.at(i));
            }

            EXPECT_EQ(md5Id,        bamRecord.ReadGroupId());
            EXPECT_EQ(movieName,    bamRecord.MovieName());
            EXPECT_EQ(numPasses,    bamRecord.NumPasses());
            EXPECT_EQ(holeNumber,   bamRecord.HoleNumber());
            EXPECT_FALSE(bamRecord.HasLocalContextFlags());
            EXPECT_FALSE(bamRecord.HasSignalToNoise());
            numTested++;
        }

cleanup:
        EXPECT_GT(numTested, 1UL);

        // cleanup
        baxReader.Close();
        RemoveFile(generatedBam);
        RemoveFile(generatedBam + ".pbi");

    }); // EXPECT_NO_THROW
}
