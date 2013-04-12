#include "kmerizer.h"
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <bitset>
#include <algorithm> // stl sort
#include <cstdlib> // qsort()
#include <cstdio>  // sprintf()
#include <cstring> // memcpy()
#include <sys/stat.h> // mkdir()
#include <ctime> // clock_t clock() CLOCKS_PER_SEC


// constructor
kmerizer::kmerizer(const size_t _k, const size_t _threads, char* _outdir, const char _mode) {
	k = _k;
	kmask = 0xFFFFFFFFFFFFFFFFULL;
	shiftlastby = 62;
	lshift=0;
	rshift=0;
	if (k%32 > 0) {
		kmask = (1ULL << (2*(k%32))) - 1;
		shiftlastby = 2*(k%32) - 2;
		lshift = 2*(k%32);
		rshift = 64 - lshift;
	}
	nwords = ((k-1)>>5)+1;
	kmer_size = nwords*sizeof(word_t);
	outdir = _outdir;
	mode = _mode;
	threads = _threads;
	thread_bins = NBINS/threads;
	state = READING;
	batches=0;
}

int kmerizer::allocate(const size_t maximem) {
	fprintf(stderr,"kmerizer::allocate(%zi)",maximem);
	clock_t start = clock();
	memset(bin_tally,0,sizeof(uint32_t)*NBINS);
	max_kmers_per_bin = maximem/kmer_size/NBINS;
	for(size_t i=0; i<NBINS; i++) {
		kmer_buf[i] = (word_t*) calloc(max_kmers_per_bin, kmer_size);
		if (kmer_buf[i]==NULL) return 1;
	}
	fprintf(stderr," took %f seconds\n",((float)(clock()-start))/CLOCKS_PER_SEC);
	
	return 0;
}

word_t* kmerizer::canonicalize(word_t *packed, word_t *rcpack) const {
	for(size_t i=0;i<nwords;i++) {
		rcpack[i] = packed[nwords-1-i];
	}
	if (lshift) {
		for(size_t i=0;i<nwords-1;i++) {
			rcpack[i] |= rcpack[i+1] << lshift;
			rcpack[i+1] >>= rshift;
		}
	}
	int cmp=0;
	for(size_t i=0;i<nwords;i++) {
		rcpack[i] = revcomp(rcpack[i]);
		if (i==nwords-1) rcpack[i] >>=rshift;
		if (cmp == 0) {
			if (packed[i] < rcpack[i])
				cmp = -1;
			else if (packed[i] > rcpack[i])
				cmp = 1;
		}
	}
	if(cmp > 0)
		return rcpack;
	else
		return packed;
}

void kmerizer::next_kmer(word_t* kmer, const char nucl) {
	// shift first kmer by 2 bits
	kmer[0] <<= 2;
	for(size_t w=1;w<nwords-1;w++) { // middle (full length) words
		kmer[w-1] |= kmer[w] >> 62; // move the top two bits
		kmer[w] <<= 2; // make room for the next word
	}
	// last word (if not the first)
	if(nwords>1) {
		kmer[nwords-2] |= (kmer[nwords-1] >> shiftlastby);
		kmer[nwords-1] <<= 2;
	}
	kmer[nwords-1] |= twobit(nucl);
	kmer[nwords-1] &= kmask;
}

void kmerizer::addSequence(const char* seq, const int length) {
	if (length < k) return;
	word_t packed [nwords];
	word_t rcpack [nwords];
	memset(packed,0,kmer_size);

	for(size_t i=0;i<k;i++)
		next_kmer(packed,seq[i]);

	word_t *kmer = packed;
	size_t bin;
	if (mode == CANONICAL)
		kmer = canonicalize(packed,rcpack);
	if (mode == BOTH) {
		kmer = canonicalize(packed,rcpack);
		bin = hashkmer(packed,0);
		memcpy(kmer_buf[bin] + nwords*bin_tally[bin], packed, kmer_size);
		bin_tally[bin]++;
		if (bin_tally[bin] == max_kmers_per_bin) serialize();
		kmer = rcpack;
	}
	bin = hashkmer(kmer,0);
	memcpy(kmer_buf[bin] + nwords*bin_tally[bin], kmer, kmer_size);
	bin_tally[bin]++;
	if (bin_tally[bin] == max_kmers_per_bin) serialize();
	// pack the rest of the sequence
	for(size_t i=k; i<(size_t)length;i++) {
		next_kmer(packed, seq[i]);
		if (mode == CANONICAL)
			kmer = canonicalize(packed,rcpack);
		if (mode == BOTH) {
			kmer = canonicalize(packed,rcpack);
			bin = hashkmer(packed,0);
			memcpy(kmer_buf[bin] + nwords*bin_tally[bin], packed, kmer_size);
			bin_tally[bin]++;
			if (bin_tally[bin] == max_kmers_per_bin) serialize();
			kmer = rcpack;
		}
		bin = hashkmer(kmer,0);
		memcpy(kmer_buf[bin] + nwords*bin_tally[bin], kmer, kmer_size);
		bin_tally[bin]++;
		if (bin_tally[bin] == max_kmers_per_bin) serialize();
	}
}

void kmerizer::save() {
	serialize();
	
	if (batches>1)
		mergeBatches();
	else
		for(size_t bin=0;bin<NBINS;bin++) {
			char ofname [100];
			char nfname [100];
			sprintf(ofname,"%s/%zi-mers.%zi.1",outdir,k,bin);
			sprintf(nfname,"%s/%zi-mers.%zi",outdir,k,bin);
			if (rename(ofname,nfname) != 0)
				perror("error renaming file");
			sprintf(ofname,"%s/%zi-mers.%zi.1.idx",outdir,k,bin);
			sprintf(nfname,"%s/%zi-mers.%zi.idx",outdir,k,bin);
			if (rename(ofname,nfname) != 0)
				perror("error renaming file");
		}
}

void kmerizer::load() {
	fprintf(stderr,"kmerizer::load()");
	clock_t start = clock();
	boost::thread_group tg;
	for(size_t i = 0; i < NBINS; i += thread_bins) {
		size_t j = (i + thread_bins > NBINS) ? NBINS : i + thread_bins;
		tg.create_thread(boost::bind(&kmerizer::do_loadIndex, this, i, j));
	}
	tg.join_all();
	state = QUERY;
	fprintf(stderr," took %f seconds\n",((float)(clock()-start))/CLOCKS_PER_SEC);
}

// given one kmer, pack it, canonicalize it, hash it, find it
uint32_t kmerizer::find(char* seq) {
	// pack it
	word_t packed [nwords];
	word_t rcpack [nwords];
	memset(packed,0,kmer_size);
	for(size_t i=0;i<k;i++)
		next_kmer(packed,seq[i]);
	word_t *kmer = packed;

	// canonicalize it
	if (mode == CANONICAL) {
		kmer = canonicalize(packed,rcpack);
	}

	//hash it
	size_t bin = hashkmer(kmer,0);

	// find it
	return find(kmer,bin);
}

uint32_t kmerizer::find(word_t *kmer, size_t bin) {
	if (kmer_buf[bin]==NULL) {
		// try to mmap the file into the kmer_buf
		// if mmap fails, do a linear scan of the file and return the associated count
	}
	// we've got the data in kmer_buf[bin]
}

void kmerizer::histogram() {
	if (batches>1)
		save();

	if (state == READING)
		uniqify();

	// merge the NBINS counts bvecs
	uint32_t todo = NBINS;
	size_t offset [NBINS];
	size_t done [NBINS];
	memset(offset,0,sizeof(size_t)*NBINS);
	memset(done,0,sizeof(size_t)*NBINS);
	uint32_t key=1; // min kmer frequency
	while (todo > 0) {
		uint32_t val = 0;
		for (size_t i=0;i<NBINS;i++) {
			if (done[i]==0) {
				if (kmer_freq[i][offset[i]] == key) {
					val += counts[i][offset[i]]->cnt();
					if (offset[i]<kmer_freq[i].size()-1) // undo the range encoding <=
						val -= counts[i][offset[i]+1]->cnt();
					offset[i]++;
					if (offset[i] == kmer_freq[i].size()) {
						done[i]=1;
						todo--;
					}
				}
			}
		}
		if (val > 0) {
			printf("%u %u\n",key,val);
		}
		key++;
	}
}

void kmerizer::serialize() {
	batches++;
	// unique the batch
	uniqify();
	// write to disk
	writeBatch();

	// zero the data
	memset(bin_tally,0,sizeof(uint32_t)*NBINS);
	for(size_t i=0;i<NBINS;i++) {
		for(size_t j=0; j<counts[i].size(); j++)
			delete counts[i][j]; // bvec32 destructor
	}
	state = READING;
}

void kmerizer::uniqify() {
	fprintf(stderr,"kmerizer::uniqify()");
	clock_t start = clock();
	boost::thread_group tg;
	for(size_t i = 0; i < NBINS; i += thread_bins) {
		size_t j = (i + thread_bins > NBINS) ? NBINS : i + thread_bins;
		tg.create_thread(boost::bind(&kmerizer::do_unique, this, i, j));
	}
	tg.join_all();
	state = QUERY;
	fprintf(stderr," took %f seconds\n",((float)(clock()-start))/CLOCKS_PER_SEC);
}

void kmerizer::writeBatch() {
	fprintf(stderr,"kmerizer::writeBatch()");
	clock_t start = clock();
	boost::thread_group tg;
	for(size_t i = 0; i < NBINS; i += thread_bins) {
		size_t j = (i + thread_bins > NBINS) ? NBINS : i + thread_bins;
		tg.create_thread(boost::bind(&kmerizer::do_writeBatch, this, i, j));
	}
	tg.join_all();
	fprintf(stderr," took %f seconds\n",((float)(clock()-start))/CLOCKS_PER_SEC);
}

void kmerizer::mergeBatches() {
	fprintf(stderr,"kmerizer::mergeBatches()");
	clock_t start=clock();
	boost::thread_group tg;
	for(size_t i=0;i<NBINS;i+=thread_bins) {
		size_t j=(i+thread_bins>NBINS) ? NBINS : i+thread_bins;
		tg.create_thread(boost::bind(&kmerizer::do_mergeBatches, this, i, j));
	}
	tg.join_all();
	batches=1;
	fprintf(stderr," took %f seconds\n",((float)(clock()-start))/CLOCKS_PER_SEC);
}


int compare_kmers1(const void *k1, const void *k2) {
	if (*(word_t*)k1 > *(word_t*)k2) return 1;
	if (*(word_t*)k1 < *(word_t*)k2) return -1;
	return 0;
}
int compare_kmers2(const void *k1, const void *k2) {
	for(size_t i=0;i<2;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}
int compare_kmers3(const void *k1, const void *k2) {
	for(size_t i=0;i<3;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}
int compare_kmers4(const void *k1, const void *k2) {
	for(size_t i=0;i<4;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}
int compare_kmers5(const void *k1, const void *k2) {
	for(size_t i=0;i<5;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}
int compare_kmers6(const void *k1, const void *k2) {
	for(size_t i=0;i<6;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}
int compare_kmers7(const void *k1, const void *k2) {
	for(size_t i=0;i<7;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}
int compare_kmers8(const void *k1, const void *k2) {
	for(size_t i=0;i<8;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}

int kmercmp(const void *k1, const void *k2, size_t nwords) {
	for(size_t i=0;i<nwords;i++) {
		if (*((word_t*)k1+i) < *((word_t*)k2+i)) return -1;
		if (*((word_t*)k1+i) > *((word_t*)k2+i)) return 1;
	}
	return 0;
}
struct kmer1_t { word_t first_word; };
struct kmer2_t { word_t first_word; };
struct kmer3_t { word_t first_word; };
struct kmer4_t { word_t first_word; };
struct kmer5_t { word_t first_word; };
struct kmer6_t { word_t first_word; };
struct kmer7_t { word_t first_word; };
struct kmer8_t { word_t first_word; };
bool operator<(const kmer1_t& a, const kmer1_t& b) {
	return a.first_word < b.first_word;
}
bool operator<(const kmer2_t& a, const kmer2_t& b) {
	for(size_t i=0;i<2;i++) {
		if (*(&a.first_word+i) < *(&b.first_word+i)) return true;
		if (*(&a.first_word+i) > *(&b.first_word+i)) return false;
	}
	return false;
}
bool operator<(const kmer3_t& a, const kmer3_t& b) {
	for(size_t i=0;i<3;i++) {
		if (*(&a.first_word+i) < *(&b.first_word+i)) return true;
		if (*(&a.first_word+i) > *(&b.first_word+i)) return false;
	}
	return false;
}
bool operator<(const kmer4_t& a, const kmer4_t& b) {
	for(size_t i=0;i<4;i++) {
		if (*(&a.first_word+i) < *(&b.first_word+i)) return true;
		if (*(&a.first_word+i) > *(&b.first_word+i)) return false;
	}
	return false;
}
bool operator<(const kmer5_t& a, const kmer5_t& b) {
	for(size_t i=0;i<5;i++) {
		if (*(&a.first_word+i) < *(&b.first_word+i)) return true;
		if (*(&a.first_word+i) > *(&b.first_word+i)) return false;
	}
	return false;
}
bool operator<(const kmer6_t& a, const kmer6_t& b) {
	for(size_t i=0;i<6;i++) {
		if (*(&a.first_word+i) < *(&b.first_word+i)) return true;
		if (*(&a.first_word+i) > *(&b.first_word+i)) return false;
	}
	return false;
}
bool operator<(const kmer7_t& a, const kmer7_t& b) {
	for(size_t i=0;i<7;i++) {
		if (*(&a.first_word+i) < *(&b.first_word+i)) return true;
		if (*(&a.first_word+i) > *(&b.first_word+i)) return false;
	}
	return false;
}
bool operator<(const kmer8_t& a, const kmer8_t& b) {
	for(size_t i=0;i<8;i++) {
		if (*(&a.first_word+i) < *(&b.first_word+i)) return true;
		if (*(&a.first_word+i) > *(&b.first_word+i)) return false;
	}
	return false;
}


void kmerizer::do_unique(const size_t from, const size_t to) {
	for(size_t bin=from; bin<to; bin++) {
		if (1) {
		// stl sort
			if (nwords==1)
				sort((kmer1_t*)kmer_buf[bin], (kmer1_t*)(kmer_buf[bin]) + bin_tally[bin]);
			if (nwords==2)
				sort((kmer2_t*)kmer_buf[bin], (kmer2_t*)(kmer_buf[bin] + bin_tally[bin]));
			if (nwords==3)
				sort((kmer3_t*)kmer_buf[bin], (kmer3_t*)(kmer_buf[bin] + bin_tally[bin]));
			if (nwords==4)
				sort((kmer4_t*)kmer_buf[bin], (kmer4_t*)(kmer_buf[bin] + bin_tally[bin]));
			if (nwords==5)
				sort((kmer5_t*)kmer_buf[bin], (kmer5_t*)(kmer_buf[bin] + bin_tally[bin]));
			if (nwords==6)
				sort((kmer6_t*)kmer_buf[bin], (kmer6_t*)(kmer_buf[bin] + bin_tally[bin]));
			if (nwords==7)
				sort((kmer7_t*)kmer_buf[bin], (kmer7_t*)(kmer_buf[bin] + bin_tally[bin]));
			if (nwords==8)
				sort((kmer8_t*)kmer_buf[bin], (kmer8_t*)(kmer_buf[bin] + bin_tally[bin]));
		}
		if (0) {
			// qsort
			switch(nwords) {
				case 1:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers1);
				case 2:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers2);
				case 3:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers3);
				case 4:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers4);
				case 5:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers5);
				case 6:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers6);
				case 7:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers7);
				case 8:
					qsort(kmer_buf[bin], bin_tally[bin], kmer_size, compare_kmers8);
			}
		}
		// uniq
		uint32_t distinct = 0;
		vector<uint32_t> tally;
		tally.push_back(1); // first kmer
		for(size_t i=1;i<bin_tally[bin];i++) {
			word_t *ith = kmer_buf[bin] + i*nwords;
			if(kmercmp(kmer_buf[bin] + distinct*nwords, ith, nwords) == 0)
				tally.back()++;
			else {
				distinct++;
				tally.push_back(1);
				memcpy(kmer_buf[bin] + distinct*nwords, ith, kmer_size);
			}
		}
//		fprintf(stderr,"uniqify[%zi] reduced from %u - %u = %u\n",bin,bin_tally[bin],distinct+1,bin_tally[bin] - distinct - 1);
		bin_tally[bin] = distinct+1;
		// create a bitmap index for the tally vector
		range_index(tally,kmer_freq[bin],counts[bin]);
	}
}

void kmerizer::print_kmer(word_t *kmer) {
	size_t bpw = 8*sizeof(word_t);
	for(size_t j=0;j<nwords;j++) {
		fprintf(stderr," ");
		for(size_t b=0;b<bpw;b++) {
			fprintf(stderr,"%d",((*(kmer+j) & (1ULL << (bpw-b-1)))) ? 1 : 0);
		}
	}
	fprintf(stderr,"\n");
}
void kmerizer::bit_slice(word_t *kmers, const size_t n, bvec32 **kmer_slices, size_t nbits) {
	// initialize WAH compressed bitvectors
	for(size_t i=0;i<nbits;i++) {
		kmer_slices[i] = new bvec32(true);
	}
	bitset<256> bbit; // for keeping track of the set bits
	size_t boff [nbits]; // beginning of the current run of 1's or 0's
	memset(boff,0,nbits*sizeof(size_t)); // initialize to 0
	unsigned int bpw = 8*sizeof(word_t); // bits per word

	// mark the set bits in the first kmer
	if (nwords>1) {
		for(size_t w=0;w<nwords;w++) {
			unsigned int count = popcount(kmers[w]);
			for(unsigned int r=1; r<=count; r++)
				bbit.set(selectbit(kmers[w],r) + w*bpw,1);
		}
		for(size_t i=1;i<n;i++) {
			// when n is large the number of different bits between kmer i and kmer i-1 is small
			// so use xor, popcount, and selectbit to identify the changed bit positions
			word_t *kmer = kmers + i*nwords;
			word_t *prev = kmer - nwords;
			for(size_t w=0;w<nwords;w++) {
				word_t x = kmer[w] ^ prev[w];
				unsigned int count = popcount(x);
				for(unsigned int r = 1; r<=count; r++) {
					unsigned int b = selectbit(x,r) + w*bpw;
					kmer_slices[b]->appendFill(bbit.test(b),i-boff[b]);
					bbit.flip(b);
					boff[b] = i;
				}
			}
		}
	}
	else { // stripped down version for nwords=1
		unsigned int count = popcount(kmers[0]);
		for(unsigned int r=1; r<=count; r++)
			bbit.set(selectbit(kmers[0],r),1);
		for(size_t i=1;i<n;i++) {
			// when n is large the number of different bits between kmer i and kmer i-1 is small
			// so use xor, popcount, and selectbit to identify the changed bit positions
			word_t x = kmers[i] ^ kmers[i-1];
			unsigned int count = popcount(x);
			for(unsigned int r = 1; r<=count; r++) {
				unsigned int b = selectbit(x,r);
				kmer_slices[b]->appendFill(bbit.test(b),i-boff[b]);
				bbit.flip(b);
				boff[b] = i;
			}
		}
	}
	// append the last runs
	for(size_t b=0; b<nbits; b++)
		if(boff[b] < n-1)
			kmer_slices[b]->appendFill(bbit.test(b),n-boff[b]);
}

void kmerizer::do_writeBatch(const size_t from, const size_t to) {
	for(size_t bin=from; bin<to; bin++) {
		// convert kmer_buf[bin] into a bit-sliced bitmap index
//		fprintf(stderr,"bin_tally[%zi]: %u\n",bin,bin_tally[bin]);
		const size_t nbits = 8*kmer_size;
		bvec32* kmer_slices [nbits];
		bit_slice(kmer_buf[bin],bin_tally[bin],kmer_slices,nbits);
		
		FILE *fp;
		if (0) {
			// open output file for kmer_buf
			char kmer_file [100];
			sprintf(kmer_file,"%s/%zi-mers.%zi.%zi",outdir,k,bin,batches);
			fp = fopen(kmer_file, "wb");
			fwrite(kmer_buf[bin],kmer_size,bin_tally[bin],fp);
			fclose(fp);
		}
		if (1) {
			// write the kmer_slices to a file
			char kmer_file [100];
			sprintf(kmer_file,"%s/%zi-mers.%zi.%zi",outdir,k,bin,batches);
			fp = fopen(kmer_file, "wb");
			size_t n_slices = 8*sizeof(word_t);
			fwrite(&n_slices,sizeof(size_t),1,fp);
			for(size_t b=0;b<n_slices;b++) {
				uint32_t c = kmer_slices[b]->cnt();
				fwrite(&c,sizeof(uint32_t),1,fp);
			}
			// fprintf(stderr,"bin_tally[%zi]: %u\n",bin,bin_tally[bin]);
			for(size_t b=0;b<n_slices;b++) {
				uint32_t *buf;
				size_t bytes = kmer_slices[b]->dump(&buf);
				// fprintf(stderr,"%zi: %zi,",b,bytes);
				fwrite(&bytes,sizeof(size_t),1,fp);
				fwrite(buf,1,bytes,fp);
				free(buf);
			}
//			fprintf(stderr,"\n");
			fclose(fp);
		}
		// open output file for counts
		char counts_file [100];
		sprintf(counts_file,"%s/%zi-mers.%zi.%zi.idx",outdir,k,bin,batches);
		fp = fopen(counts_file, "wb");
		// first write the number of distinct values
		// then write the distinct values
		// for each distinct value, write out the bvec (size,count,rle,words.size(),words)
		size_t n_distinct = kmer_freq[bin].size();
		fwrite(&n_distinct,sizeof(size_t),1,fp);
		fwrite(kmer_freq[bin].data(),sizeof(uint32_t),n_distinct,fp);
		for(size_t i=0;i<n_distinct;i++) {
			uint32_t *buf;
			size_t bytes = counts[bin][i]->dump(&buf);
			// how many bytes are we writing
			fwrite(&bytes,sizeof(size_t),1,fp);
			// write them
			fwrite(buf,1,bytes,fp);
			free(buf);
		}
		fclose(fp);
	}
}

// when reading counts, values is the array of kmer_frequencies
// when reading kmers, values is the number of set bits in each bit slice
void kmerizer::read_bitmap(const char* idxfile, vector<uint32_t> &values, vector<bvec32*> &index) {
	// open idxfile
	FILE *fp;
	fp = fopen(idxfile,"rb");
	// fread to repopulate values and bvecs
	size_t n_distinct;
	fread(&n_distinct,sizeof(size_t),1,fp);
	values.resize(n_distinct);
	fread(values.data(),sizeof(uint32_t),n_distinct,fp);
	index.resize(n_distinct);
	for(size_t i=0;i<n_distinct;i++) {
		size_t bytes;
		fread(&bytes,sizeof(size_t),1,fp);
		size_t words = bytes/sizeof(uint32_t);
		uint32_t buf [words];
		fread(buf,1,bytes,fp);
		index[i] = new bvec32(buf);
	}
	fclose(fp);
}

void kmerizer::do_loadIndex(const size_t from, const size_t to) {
	for(size_t bin=from; bin<to;bin++) {
		char fname [100];
		sprintf(fname,"%s/%zi-mers.%zi.idx",outdir,k,bin);
		read_bitmap(fname,kmer_freq[bin],counts[bin]);
	}
}

void kmerizer::do_mergeBatches(const size_t from, const size_t to) {
	for(size_t bin=from; bin<to; bin++) {
		// read the counts and kmers for each batch
		vector<bvec32*> batch_counts [batches];
		vector<uint32_t> batch_values [batches];
		vector<bvec32*> batch_slices [batches];
		vector<uint32_t> batch_slice_cnts [batches];
		for(size_t i=0;i<batches;i++) {
			char fname [100];
			sprintf(fname,"%s/%zi-mers.%zi.%zi",outdir,k,bin,i+1);
			read_bitmap(fname,batch_slice_cnts[i],batch_slices[i]);
			sprintf(fname,"%s/%zi-mers.%zi.%zi.idx",outdir,k,bin,i+1);
			read_bitmap(fname,batch_values[i],batch_counts[i]);
		}

		word_t kmers [batches*nwords]; // next kmer in each active batch
		uint32_t btally [batches]; // frequency of next kmer in each batch
		size_t offset [batches]; // keep track of position in each batch
		memset(offset,0,sizeof(size_t)*batches);
		uint32_t todo = batches; // number of batches to process
		vector<uint32_t> tally;

		const size_t nbits = 8*kmer_size;
		bvec32* merged_slices [nbits];
		bitset<256> bbit;
		unsigned int boff [nbits];
		unsigned int n=0;
		memset(boff,0,nbits*sizeof(size_t));
		const unsigned int bpw = 8*sizeof(word_t); // bits per word

		for(size_t b=0;b<nbits;b++)
			merged_slices[b] = new bvec32(true);

		// read the first kmer and counts from each batch
		for(size_t i=0;i<batches;i++) {
			size_t err = pos2kmer(offset[i],kmers + i*nwords, batch_slices[i]);
			if (err != 0) {
				todo--;
				btally[i]=0;
			}
			else {
				btally[i] = pos2value(offset[i]++,batch_values[i],batch_counts[i]);
			}
		}
		// choose min
		size_t mindex = find_min(kmers,btally);
		word_t distinct [nwords];
		memcpy(distinct,kmers + mindex*nwords, kmer_size);
		tally.push_back(btally[mindex]);
		// mark the set bits in the first distinct kmer
		for(size_t w=0;w<nwords;w++) {
			unsigned int count = popcount(distinct[w]);
			for(unsigned int r=1; r<=count; r++)
				bbit.set(selectbit(distinct[w],r)+w*bpw,1);
		}
		// replace min
		// iterate until there's nothing left to do
		
		while(todo>0) {
			size_t err = pos2kmer(offset[mindex],kmers + mindex*nwords, batch_slices[mindex]);
			if (err != 0) {
				todo--;
				btally[mindex]=0;
				if (todo==0) break;
			}
			else
				btally[mindex] = pos2value(offset[mindex]++,batch_values[mindex],batch_counts[mindex]);
			mindex = find_min(kmers,btally);
			// compare to distinct
			if (kmercmp(kmers + mindex*nwords, distinct, nwords) == 0) // same kmer
				tally.back() += btally[mindex];
			else { // find the changed bits
				tally.push_back(btally[mindex]);
				n++;
				word_t *minkmer = kmers + mindex*nwords;
				for(size_t w=0;w<nwords;w++) {
					word_t x = distinct[w] ^ minkmer[w];
					unsigned int count = popcount(x);
					for(unsigned int r = 1; r<=count; r++) {
						unsigned int b = selectbit(x,r) + w*bpw;
						merged_slices[b]->appendFill(bbit.test(b),n-boff[b]);
						bbit.flip(b);
						boff[b]=n;
					}
					distinct[w] = minkmer[w];
				}
			}
		}
		// finish the bitvectors
		n++;
		for(size_t b=0; b<nbits; b++)
			if(boff[b]<n-1)
				merged_slices[b]->appendFill(bbit.test(b),n-boff[b]);
		
		range_index(tally,kmer_freq[bin],counts[bin]);

		// open an output file for the merged distinct kmers
		FILE *ofp;
		char fname [100];
		sprintf(fname,"%s/%zi-mers.%zi",outdir,k,bin);
		ofp = fopen(fname,"wb");

		fwrite(&nbits,sizeof(size_t),1,ofp);
		for(size_t b=0;b<nbits;b++) {
			uint32_t c = merged_slices[b]->cnt();
			fwrite(&c,sizeof(uint32_t),1,ofp);
		}
		for(size_t b=0;b<nbits;b++) {
			uint32_t *buf;
			size_t bytes = merged_slices[b]->dump(&buf);
			// fprintf(stderr,"%zi: %zi,",b,bytes);
			fwrite(&bytes,sizeof(size_t),1,ofp);
			fwrite(buf,1,bytes,ofp);
			free(buf);
		}


		fclose(ofp);
		
		char counts_file [100];
		sprintf(counts_file,"%s/%zi-mers.%zi.idx",outdir,k,bin);
		ofp = fopen(counts_file, "wb");
		// first write the number of distinct values
		// then write the distinct values
		// for each distinct value, write out the bvec (size,count,rle,words.size(),words)
		int n_distinct = kmer_freq[bin].size();
		fwrite(&n_distinct,sizeof(int),1,ofp);
		fwrite(kmer_freq[bin].data(),sizeof(uint32_t),n_distinct,ofp);
		for(size_t i=0;i<n_distinct;i++) {
			// I want this DIY serialization/deserialization in bvec
			uint32_t *buf;
			size_t bytes = counts[bin][i]->dump(&buf);
			fwrite(&bytes,sizeof(size_t),1,ofp);
			fwrite(counts[bin][i]->get_words().data(),1,counts[bin][i]->bytes(),ofp);
		}
		fclose(ofp);
		// delete input files
		// for(size_t i=0;i<batches;i++) {
		// 	sprintf(fname,"%s/%zi-mers.%zi.%zi",outdir,k,bin,i+1);
		// 	if (remove(fname) != 0) perror("error deleting file");
		// 	sprintf(fname,"%s/%zi-mers.%zi.%zi.idx",outdir,k,bin,i+1);
		// 	if (remove(fname) != 0) perror("error deleting file");
		// }
	}
}

inline size_t kmerizer::find_min(const word_t* kmers, const uint32_t* kcounts) {
	size_t mindex = 0;
	while (kcounts[mindex] == 0) mindex++;
	for(size_t i=mindex+1;i<batches;i++)
		if (kcounts[i] != 0)
			if (kmercmp(kmers + i*nwords,kmers + mindex*nwords,nwords) < 0)
				mindex = i;

	return mindex;
}

// for each distinct value in the vec create a bitvector
// indexing the positions in the vec holding a value <= v
void kmerizer::range_index(vector<uint32_t> &vec, vector<uint32_t> &values, vector<bvec32*> &index) {
	// find the distinct values in vec
	values.clear();
	// use a bitset to mark the distinct values (from 0-255)
	// and another vector for overflow (which we'll sort/uniq later)
	bitset<256> mybits;
	vector<uint32_t> overflow;
	vector<uint32_t>::iterator it;
	for(it = vec.begin(); it < vec.end(); ++it) {
		if (*it >= 256) 
			overflow.push_back(*it);
		else
			mybits[*it]=1;
	}
	// populate values vector with set bits in mybits
	for(uint32_t i=0;i<256;i++)
		if (mybits.test(i))
			values.push_back(i);
	
	if (overflow.size() > 0) {
		sort(overflow.begin(),overflow.end());
		it = unique(overflow.begin(),overflow.end());
		values.insert(values.end(),overflow.begin(),it);
	}

	// setup a vector for each range
	vector<uint32_t> vrange [values.size()];
	// iterate over the vec and push the offset onto each range
	for(size_t i=0;i<vec.size();i++) {
		it = lower_bound(values.begin(),values.end(),vec[i]);
		for(size_t j=0;j<=(it - values.begin());j++)
			vrange[j].push_back(i);
	}
	// create a bitvector for each range
	index.resize(values.size());
	for(size_t i=0;i<values.size();i++)
		index[i] = new bvec32(vrange[i]);
}

