#ifndef KMERIZER_H
#define KMERIZER_H

#define DEBUG false
#define NBINS 256
#define CANONICAL 'C'
#define BOTH 'B'
#define READING 1
#define QUERY 2

#include <vector>
#include "bvec32.h"

typedef uint64_t word_t;
using namespace std;

class kmerizer {
	size_t k;
	word_t kmask;
	size_t shiftlastby;
	size_t rshift, lshift;
	size_t nwords;
	size_t kmer_size; // in bytes
	size_t threads;
	size_t thread_bins;
	size_t batches;
	size_t max_kmers_per_bin;
	char mode;
	char state;
	char *outdir;

	word_t* kmer_buf[NBINS]; // raw unsorted padded kmers, or sort|uniq'ed kmers
	uint32_t bin_tally[NBINS]; // number of kmers in each bin (or number of distinct kmers)
	vector<uint32_t> kmer_freq [NBINS]; // sorted distinct kmer frequencies
	vector<bvec32*> counts [NBINS]; // bitmap index of frequency counts

public:
	kmerizer(const size_t _k, const size_t _threads, char* _outdir, const char _mode);
	int allocate(const size_t maximem); // allocates memory for each kmer_buf
	void addSequence(const char* seq,const int length); // extract (canonicalized) kmers from the sequence
	void save(); // writes distinct kmers and rle counts to disk (merging multiple batches)
	void load(); // reads kmer indexes into memory
	void histogram(); // output the kmer count frequency distribution
	uint32_t find(char* query);
	~kmerizer() {};

private:
	inline word_t twobit(const word_t val) const; // pack nucleotides into 2 bits
	void next_kmer(word_t* kmer, const char nucl); // shift kmer to make room for nucl
	inline void unpack(word_t* kmer, char *seq);
	inline word_t revcomp(const word_t val) const; // reverse complement
	inline uint8_t hashkmer(const word_t *kmer, const uint8_t seed) const; // to select a bin
	word_t* canonicalize(word_t *packed, word_t *rcpack) const;
	uint32_t find(word_t *kmer, size_t bin);
	void serialize(); // kmer_buf is full. uniqify and write batch to disk
	void uniqify(); // qsort each kmer_buf, update bin_tally, and fill counts
	void do_unique(const size_t from, const size_t to); // for parallelization
	void writeBatch();
	void do_writeBatch(const size_t from, const size_t to); // for parallelization
	void mergeBatches();
	void do_mergeBatches(const size_t from, const size_t to);
	void do_loadIndex(const size_t from, const size_t to);
	// is this too generic to go here?
	void range_index(vector<uint32_t> &vec, vector<uint32_t> &values, vector<bvec32*> &index);
	void read_index(const char* idxfile,vector<uint32_t> &values, vector<bvec32*> &index);
	size_t find_min(const word_t* kmers, const uint32_t* kcounts);
	uint32_t pos2value(size_t pos, vector<uint32_t> &values, vector<bvec32*> &index);
	void print_kmer(word_t *kmer);
	void bit_slice(word_t *kmers, const size_t n, bvec32 **kmer_slices, size_t nbits);
};

inline word_t kmerizer::twobit(const word_t val) const {
	static const word_t table[256] =
	{
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,1,0,0,0,2,0,0,0,0,0,0,0,0,
		0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,1,0,0,0,2,0,0,0,0,0,0,0,0,
		0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
	};
	return table[val];
}

inline void kmerizer::unpack(word_t* kmer, char* seq) {
	static const char table[4] = {65, 67, 71, 84};
	for(size_t i=0;i<k;i++) {
		// which word, which nucl
		size_t w = i>>5;
		if (w == nwords-1) // not a full length word
			seq[i] = table[(kmer[w] >> 2*(k-i)-2) & 3];
		else
			seq[i] = table[(kmer[w] >> (62 - 2*(i%32))) & 3];
	}
	seq[k] = '\0';
}

inline word_t kmerizer::revcomp(const word_t val) const {
	// bitwise reverse complement of values from 0 to 255
	static const word_t table[256] =
	{
		255,191,127,63,239,175,111,47,223,159,95,31,207,143,79,15,
		251,187,123,59,235,171,107,43,219,155,91,27,203,139,75,11,
		247,183,119,55,231,167,103,39,215,151,87,23,199,135,71,7,
		243,179,115,51,227,163,99,35,211,147,83,19,195,131,67,3,
		254,190,126,62,238,174,110,46,222,158,94,30,206,142,78,14,
		250,186,122,58,234,170,106,42,218,154,90,26,202,138,74,10,
		246,182,118,54,230,166,102,38,214,150,86,22,198,134,70,6,
		242,178,114,50,226,162,98,34,210,146,82,18,194,130,66,2,
		253,189,125,61,237,173,109,45,221,157,93,29,205,141,77,13,
		249,185,121,57,233,169,105,41,217,153,89,25,201,137,73,9,
		245,181,117,53,229,165,101,37,213,149,85,21,197,133,69,5,
		241,177,113,49,225,161,97,33,209,145,81,17,193,129,65,1,
		252,188,124,60,236,172,108,44,220,156,92,28,204,140,76,12,
		248,184,120,56,232,168,104,40,216,152,88,24,200,136,72,8,
		244,180,116,52,228,164,100,36,212,148,84,20,196,132,68,4,
		240,176,112,48,224,160,96,32,208,144,80,16,192,128,64,0,
	};
	
	return
		(table[val&0xFFUL]<<56) |
		(table[(val>>8)&0xFFUL]<<48) |
		(table[(val>>16)&0xFFUL]<<40) |
		(table[(val>>24)&0xFFUL]<<32) |
		(table[(val>>32)&0xFFUL]<<24) |
		(table[(val>>40)&0xFFUL]<<16) |
		(table[(val>>48)&0xFFUL]<<8) |
		(table[(val>>56)&0xFFUL]);
}

inline uint8_t kmerizer::hashkmer(const word_t *kmer, const uint8_t seed) const {
	static const uint8_t Rand8[256] =
	{
		105,193,195, 26,208, 80, 38,156,128,  2,101,205, 75,116,139, 61,
		197,120,244, 51,185,132, 55,150,177,241,103,196, 13,237,136, 24,
		211, 56,207,  9, 30,145, 18,167,108, 32,106,151,  3, 54,248, 65,
		 17,198, 85, 95, 29, 83,  4,206,188,186,107,255,129, 35,142, 91,
		203,158, 74,138,162,135,102,114, 81,170, 10, 19,215, 57,214, 70,
		 37,163,231,227,152, 14, 40, 84, 68,252,  0, 66,121,127,223, 78,
		201, 34,225,240,124,191, 12, 42, 92,209,  1, 31,130,100, 28,224,
		161,249,110, 77, 87,144,181, 21, 86, 58,174,113,194,147,242, 50,
		 59, 48,250, 88,184,245, 45, 44,148, 73,154,230,149, 89,118,119,
		 79,229,239,117,189,179,254,155, 20,176,157,212, 36,123,234, 46,
		159,202,171, 67, 93, 62, 47,164,247, 15,137,235,216,160,200,133,
		140,172,192,221,131,111,218,210,153,219, 41, 72, 63,183, 39,  8,
		 98,168, 52,213,175,134,115, 90, 82, 16,226,220,251, 69,243,233,
		180,  5, 99, 60,  7,204,112,253,182,109, 25, 53, 94,187,190,178,
		 97, 49,126,  6, 64,143,169,173, 43,199, 33, 96, 76, 11,236, 23,
		166,104,141,246,125,217,122,238, 27, 22,228,146,165, 71,232,222
	};
	uint8_t h=seed;
	for(size_t i=0;i<nwords;i++) {
		h = Rand8[h ^ (uint8_t)(kmer[i]>>56)];
		h = Rand8[h ^ (uint8_t)(kmer[i]>>48) & 255];
		h = Rand8[h ^ (uint8_t)(kmer[i]>>40) & 255];
		h = Rand8[h ^ (uint8_t)(kmer[i]>>32) & 255];
		h = Rand8[h ^ (uint8_t)(kmer[i]>>24) & 255];
		h = Rand8[h ^ (uint8_t)(kmer[i]>>16) & 255];
		h = Rand8[h ^ (uint8_t)(kmer[i]>>8) & 255];
		h = Rand8[h ^ (uint8_t)kmer[i] & 255];
	}
	return h;
}


#endif
