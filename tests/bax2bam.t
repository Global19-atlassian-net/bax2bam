
Simple test to make sure bax2bam runs properly.
#  $ cd $WORKSPACE
#  $ cd smrtanalysis/_output/modulebuilds/bioinformatics/staging/PostPrimary/bax2bam/_output/install/binwrap-build

BAX2BAM=? MUST SET UP PATH TO BAX2BAM

  $ rm -f tst1.*.bam
  $ $BAX2BAM -o tst1 /pbi/dept/secondary/siv/testdata/bax2bam/m160823_221224_ethan_c010091942559900001800000112311890_s1_p0.1.bax.h5
