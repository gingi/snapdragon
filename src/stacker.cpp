#include "ibis.h"
#include "bord.h"	// ibis::bord, ibis::table::bufferList
#include <map>
#include <string>
#include <boost/thread.hpp>

// had to make these global
const char* Afrom = 0;
const char* Aqcnd="1=1";
char* Asel;
const char* Astart="start";
const char* Aend = "end";
ibis::table::stringList Anames;
ibis::table::typeList Atypes;
ibis::table::stringList Acols;
std::map<const char*,ibis::TYPE_T> Anaty;
ibis::qExpr* Acond=0;

const char* Bfrom = 0;
const char* Bqcnd="1=1";
char* Bsel;
const char* Bstart="start";
const char* Bend = "end";
ibis::table::stringList Bcols;
ibis::table::stringList Bnames;
ibis::table::typeList Btypes;
std::map<const char*,ibis::TYPE_T> Bnaty;
ibis::qExpr* Bcond=0;

int left=1000;
int right=1000;
int same_strand=0;
int stranded_windows=0;
int bins=0;

int parallelize=0;

ibis::whereClause senseWhere = ibis::whereClause("strand == 1");
ibis::qExpr* senseExpr = senseWhere.getExpr();

std::vector<ibis::table::bufferList*> part_results;

// printout the usage string
static void usage(const char* name)
{
  std::cout << "usage:\n" << name << std::endl
	    << "[-d1 directory containing A dataset] " << std::endl
	    << "[-c1 columns from A]" << std::endl
	    << "[-w1 where-clause for A]" << std::endl
		<< "[-s1 start column from A]" << std::endl
		<< "[-e1 end column from A]" << std::endl
	    << "[-d2 directory containing B dataset] " << std::endl
	    << "[-c2 columns from B]" << std::endl
	    << "[-w2 where-clause for B]" << std::endl
		<< "[-s2 start column from B]" << std::endl
		<< "[-e2 end column from B]" << std::endl
		<< "[-l bases to the left of A features]" << std::endl
		<< "[-r bases to the right of A features]" << std::endl
		<< "[-sm only report hits in B that overlap A on the same strand]" << std::endl
		<< "[-sw define -l and -r based on strand]" << std::endl
		<< "[-b number of bins to use for normalization after stacking. default: don't normalize]" << std::endl
		<< "[-p parallelize using threads]" << std::endl;
}

// function to parse the command line arguments
void parse_args(int argc, char** argv)
{
	for (size_t i=1; i<argc; ++i) {
	    if (*argv[i] == '-') { // arguments starting with -
	      switch (argv[i][1]) {
		      default:
		      case 'h':
		      case 'H':
				usage(*argv);
				exit(0);
			  case 'p':
				  parallelize=1;
				  break;
	  		  case 'b':
	  			  if(i+1 < argc) {
	  				  ++i;
	  				  bins = atoi(argv[i]);
	  			  }
	  			  break;
			  case 'l':
				  if(i+1 < argc) {
					  ++i;
					  left = atoi(argv[i]);
				  }
				  break;
			  case 'r':
				  if(i+1 < argc) {
					  ++i;
					  right = atoi(argv[i]);
				  }
				  break;
		      case 'd':
		      case 'D':
				if (i+1 < argc) {
				  if (argv[i][2] == '1')
					  Afrom = argv[++i];
				  else
					  Bfrom = argv[++i];
				}
				break;
	  	      case 'c':
	  			if (i+1 < argc) {
	  				if (argv[i][2] == '1')
						Asel = argv[++i];
					else
						Bsel = argv[++i];
					break;
				}
				break;
		      case 'e':
				if (i+1 < argc) {
					if (argv[i][2] == '1')
						Aend = argv[++i];
					else
						Bend = argv[++i];
	  				break;
	  			}
	  			break;
		      case 's':
				switch (argv[i][2]) {
					case '1':
						Astart = argv[++i];
						break;
					case '2':
						Bstart = argv[++i];
						break;
					case 'm':
						same_strand = 1;
						break;
					case 'w':
						stranded_windows = 1;
						break;
				}
				break;
		      case 'w':
		      case 'W':
				if (i+1 < argc) {
				  if (argv[i][2] == '1')
					  Aqcnd = argv[++i];
				  else
					  Bqcnd = argv[++i];
				}
				break;
	      } // switch (argv[i][1])
	    } // arguments starting with -
	} // for (i=1; ...)

	std::cerr << *argv << " -d1 " << Afrom << " -d2 " << Bfrom
	  << " -s1 " << Astart << " -s2 " << Bstart
	  << " -e1 " << Aend << " -e2 " << Bend
	  << " -w1 " << Aqcnd << " -w2 " << Bqcnd
	  << " -c1 " << Asel << " -c2 " << Bsel
	  << " -r " << right << " -l " << left
	  << " -b " << bins << " -p " << parallelize
	  << " -sm " << same_strand << " -sw " << stranded_windows
	  << std::endl;
}

// function to fill an array with values from an input array based on a list of indices
// Similar to ibis::util::reorder, but ind.size() can be > arr.size()
// because indices can be repeated.
template <typename T>
void meorder(ibis::array_t<T> &res, ibis::array_t<T> &arr, ibis::array_t<uint32_t>& ind)
{
	for (size_t i = 0; i < ind.size(); ++ i)
	    res[i] = arr[ind[i]];
}

// string version of meorder
void meorder(std::vector<std::string> &res, std::vector<std::string> &arr,
	ibis::array_t<uint32_t>& ind)
{
	for(size_t i = 0; i < ind.size(); ++ i)
		res[i] = arr[ind[i]];
}

// given a column, a bitvector mask, and an index
// fetch the relevant rows in the matching format
// and store in result
void fillColumn(ibis::column* col, ibis::bitvector* mask,
	ibis::array_t<uint32_t> &idx, void* result)
{
	switch(col->type()) {
		case ibis::BYTE:
		{
			ibis::array_t<signed char> *values = col->selectBytes(*mask);
			meorder(*static_cast<ibis::array_t<signed char>*>(result),*values,idx);
			break;
		}
		case ibis::UBYTE:
		{
			ibis::array_t<unsigned char> *values = col->selectUBytes(*mask);
			meorder(*static_cast<ibis::array_t<unsigned char>*>(result),*values,idx);
			break;
		}
		case ibis::SHORT:
		{
			ibis::array_t<int16_t> *values = col->selectShorts(*mask);
			meorder(*static_cast<ibis::array_t<int16_t>*>(result),*values,idx);
			break;
		}
		case ibis::USHORT:
		{
			ibis::array_t<uint16_t> *values = col->selectUShorts(*mask);
			meorder(*static_cast<ibis::array_t<uint16_t>*>(result),*values,idx);
			break;
		}
		case ibis::INT:
		{
			ibis::array_t<int32_t> *values = col->selectInts(*mask);
			meorder(*static_cast<ibis::array_t<int32_t>*>(result),*values,idx);
			break;
		}
		case ibis::UINT:
		{
			ibis::array_t<uint32_t> *values = col->selectUInts(*mask);
			meorder(*static_cast<ibis::array_t<uint32_t>*>(result),*values,idx);
			break;
		}
		case ibis::LONG:
		{
			ibis::array_t<int64_t> *values = col->selectLongs(*mask);
			meorder(*static_cast<ibis::array_t<int64_t>*>(result),*values,idx);
			break;
		}
		case ibis::ULONG:
		{
			ibis::array_t<uint64_t> *values = col->selectULongs(*mask);
			meorder(*static_cast<ibis::array_t<uint64_t>*>(result),*values,idx);
			break;
		}
		case ibis::FLOAT:
		{
			ibis::array_t<float> *values = col->selectFloats(*mask);
			meorder(*static_cast<ibis::array_t<float>*>(result),*values,idx);
			break;
		}
		case ibis::DOUBLE:
		{
			ibis::array_t<double> *values = col->selectDoubles(*mask);
			meorder(*static_cast<ibis::array_t<double>*>(result),*values,idx);
			break;
		}
		case ibis::TEXT:
		case ibis::CATEGORY:
		{
			std::vector<std::string> *values = col->selectStrings(*mask);
			meorder(*static_cast<std::vector<std::string>*>(result),*values,idx);
			break;
		}
		default:
			break;
	}
}

// Build a table to represent the interval-join of two partitions
// using two bitvector masks and two arrays of offsets into the matching rows
// New columns for relative start and end are included (possibly binned)
// In the end the table needs to be sorted by these columns
// When Bstart == Bend you don't add an end column
void fillResult(const ibis::part* Apart, const ibis::part* Bpart,
	ibis::bitvector* Amatch, ibis::bitvector* Bmatch,
	ibis::array_t<uint32_t>& Aidx, ibis::array_t<uint32_t>& Bidx,
	ibis::array_t<int32_t> &relativeStart, ibis::array_t<int32_t> &relativeEnd)
{
	size_t nrows = Aidx.size();
	size_t ncols = Acols.size() + Bcols.size() + 2;
	ibis::table::bufferList * tbuff = new ibis::table::bufferList(ncols);
	part_results.push_back(tbuff);
	ibis::table::typeList ttypes(ncols);

	(*tbuff)[0] = &relativeStart;
	(*tbuff)[1] = &relativeEnd;

	boost::thread_group tg;
	size_t j=2;
	for(size_t i=0; i<Acols.size(); i++) {
		ibis::column *col = Apart->getColumn(Acols[i]);
		ttypes[j] = col->type();
		(*tbuff)[j] = ibis::table::allocateBuffer(col->type(),nrows);
		if(parallelize > 0)
			tg.create_thread(boost::bind(fillColumn,col,Amatch,Aidx,(*tbuff)[j]));
		else
			fillColumn(col,Amatch,Aidx,(*tbuff)[j]);
		j++;
	}
	for(size_t i=0; i<Bcols.size(); i++) {
		ibis::column *col = Bpart->getColumn(Bcols[i]);
		ttypes[j] = col->type();
		(*tbuff)[j] = ibis::table::allocateBuffer(col->type(),nrows);
		if(parallelize > 0)
			tg.create_thread(boost::bind(fillColumn,col,Bmatch,Bidx,(*tbuff)[j]));
		else
			fillColumn(col,Bmatch,Bidx,(*tbuff)[j]);
		j++;
	}
	if(parallelize > 0)
		tg.join_all();
}

// calculate the position of from and to relative to before_ and after_
// convert the relative start and end to bins if requested
// trim the ends if they don't fit in the binned window.
void push_relative_position(ibis::array_t<int32_t> &relativeStart,ibis::array_t<int32_t> &relativeEnd,
	bool flipped, uint32_t &before, uint32_t &after, uint32_t &from, uint32_t &to)
{
	int32_t startDiff, endDiff;
	if (flipped) {
		startDiff = after - to;
		endDiff = after - from;
	} else {
		startDiff = from - before;
		endDiff = to - before;
	}
	if (bins > 0) {
		int32_t winlength = after - before;
		startDiff = (startDiff <= 0) ? 0 : startDiff/winlength;
		endDiff = (endDiff >= winlength) ? winlength-1 : endDiff/winlength;
	}
	relativeStart.push_back(startDiff);
	relativeEnd.push_back(endDiff);
}

// fetch intervals from the partitions to be joined
// identify features that overlap
// set bits in Amatch and Bmatch so we can fetch only elements that overlap
// fill two arrays of indexes that say what rows to take from selected columns of A and B
void Stacker(const ibis::part* Apart, const ibis::part* Bpart,
	ibis::bitvector &Amask, ibis::bitvector &Bmask, int before, int after, bool flipped)
{
	if (Amask.cnt() == 0 || Bmask.cnt() == 0)
		return;

	ibis::column *Astart_col = Apart->getColumn(Astart);
	ibis::column *Aend_col = Apart->getColumn(Aend);
	ibis::column *Bstart_col = Bpart->getColumn(Bstart);
	ibis::column *Bend_col = Bpart->getColumn(Bend);

	ibis::array_t<ibis::rid_t> *Arids = Apart->getRIDs(Amask);
	ibis::array_t<ibis::rid_t> *Brids = Bpart->getRIDs(Bmask);
	ibis::array_t<uint32_t> *Astart_val = Astart_col->selectUInts(Amask);
	ibis::array_t<uint32_t> *Aend_val = Aend_col->selectUInts(Amask);
	ibis::array_t<uint32_t> *Bstart_val = Bstart_col->selectUInts(Bmask);
	ibis::array_t<uint32_t> *Bend_val = Bend_col->selectUInts(Bmask);


	// fill the relativeStart and relativeEnd arrays - normalized if requested
	// pass these to fillResult() so we can include them with the user-requested columns
	ibis::array_t<int32_t> * relativeStart = new ibis::array_t<int32_t>();
	ibis::array_t<int32_t> * relativeEnd = new ibis::array_t<int32_t>();
	
	ibis::bitvector* Amatch;
	ibis::bitvector* Bmatch;
	Amatch = new ibis::bitvector;
	Bmatch = new ibis::bitvector;
	const uint32_t nA = Amask.cnt();
	const uint32_t nB = Bmask.cnt();
	ibis::array_t<uint32_t> Aidx;
	ibis::array_t<uint32_t> Bidx;
	uint32_t iA=0;
	uint32_t iB=0;
	uint32_t mA=0;
	uint32_t mB=0;

	std::map<uint32_t,bool> Bseen;
	while (iA < nA && iB < nB) {
		uint32_t before_ = (*Astart_val)[iA] - before;
		uint32_t after_ = (*Aend_val)[iA] + after;
		if ((*Bend_val)[iB] < before_) {// B comes first
			if(Bseen.count(iB)>0)
				mB++;
			iB++;
		}
		else if (after_ < (*Bstart_val)[iB]) // A comes first
			iA++;
		else {
			// found a match
			Aidx.push_back(mA);
			Bidx.push_back(mB);
			push_relative_position(*relativeStart,*relativeEnd,flipped,before_,after_,(*Bstart_val)[iB],(*Bend_val)[iB]);

			Amatch->setBit((*Arids)[iA].value, 1);
			if (Bseen.count(iB) == 0) {
				Bseen.insert(std::pair<uint32_t,bool>(iB,true));
				Bmatch->setBit((*Brids)[iB].value, 1);
			}
			// report all the matches of B within this A interval
			// then increment iA
			uint32_t iiB=iB+1;
			uint32_t mmB=mB+1;
			while (iiB < nB && (*Bstart_val)[iiB] <= after_) {
				if ((*Bend_val)[iiB] >= before_) {
					// overlap
					Aidx.push_back(mA);
					Bidx.push_back(mmB);
					push_relative_position(*relativeStart,*relativeEnd,flipped,before_,after_,(*Bstart_val)[iiB],(*Bend_val)[iiB]);
					if (Bseen.count(iiB) == 0) {
						Bseen.insert(std::pair<uint32_t,bool>(iiB,true));
						Bmatch->setBit((*Brids)[iiB].value, 1);
					}
					mmB++;
				}
				iiB++;
			}
			mA++;
			iA++;
		}
	}

	Amatch->adjustSize(0,Apart->nRows());
	Bmatch->adjustSize(0,Bpart->nRows());

	Amatch->compress();
	Bmatch->compress();
	if (Amatch->cnt() == 0 || Bmatch->cnt() == 0)
		return;

	fillResult(Apart,Bpart,Amatch,Bmatch,Aidx,Bidx,*relativeStart,*relativeEnd);
}

// apply the user supplied query expression to the given part
// populate a bitvector mask that corresponds to the matching rows
// return the number of rows that match
int countHits(const ibis::part* part, ibis::bitvector &mask,
	 const ibis::qExpr* cond, const char* colName)
{
	if (cond != 0) {
		ibis::countQuery que(part);
		int ierr = que.setWhereClause(cond);
		ierr = que.evaluate();
		mask.copy(*que.getHitVector());
	}
	else {
		ibis::column *c = part->getColumn(colName);
		c->getNullMask(mask);
	}
	return mask.cnt();
}

// populate bitvectors for hits to the sense and antisense strands
// Behold the beauty of WAH bitwise operations!
void splitByStrand(const ibis::part* part, ibis::bitvector &mask,
	ibis::bitvector &plus, ibis::bitvector &minus)
{
	ibis::countQuery que(part);
	int ierr = que.setWhereClause(senseExpr);
	ierr = que.evaluate();
	plus.copy(*que.getHitVector());
	minus.copy(*que.getHitVector());
	minus.flip();
	plus &= mask;
	minus &= mask;
}

// intersecting genomic intervals are always on the same chromsome
// so we operate on partitions with the same FBchr
// other command line options control how intervals are selected
// so we have to handle special cases related to strand
void setupStacker(const ibis::part* Apart, const ibis::part* Bpart)
{
	// TODO: do some checks
	// count hits to A and B on this chr
	// make masks based on Aqcnd and Bqcnd
    ibis::bitvector Amask;
	int Arows = countHits(Apart, Amask, Acond, Astart);	
	if (Arows == 0) return;
	
    ibis::bitvector Bmask;
	int Brows = countHits(Bpart, Bmask, Bcond, Bstart);
	if (Brows == 0) return;
	
	if(same_strand == 1 && (Anaty.count("strand") > 0) && (Bnaty.count("strand") > 0)) {
		// split into subproblems for each strand
		// update the masks (Aplus, Aminus, Bplus, Bminus)
		ibis::bitvector Aplus;
		ibis::bitvector Aminus;
		splitByStrand(Apart, Amask, Aplus, Aminus);
		
		ibis::bitvector Bplus;
		ibis::bitvector Bminus;
		splitByStrand(Bpart, Bmask, Bplus, Bminus);

		Stacker(Apart, Bpart, Aplus, Bplus, left, right, false);
		if (stranded_windows>0) {
			Stacker(Apart, Bpart, Aminus, Bminus, right, left, true);
		}
		else {
			Stacker(Apart, Bpart, Aminus, Bminus, left, right, false);
		}
	}
	else {
		if (stranded_windows>0 && left != right and Anaty.count("strand")>0) {
			// split into subproblems
			// update the masks (Aplus, Aminus)
			ibis::bitvector Aplus;
			ibis::bitvector Aminus;
			splitByStrand(Apart, Amask, Aplus, Aminus);
			Stacker(Apart, Bpart, Aplus, Bmask, left, right, false);
			Stacker(Apart, Bpart, Aminus, Bmask, right, left, true);
		}
		else {
			Stacker(Apart, Bpart, Amask, Bmask, left, right, false);
		}
	}
}

// make a list of valid column names from the given select string
void fillColumnLists(char * sel, std::map<const char*,ibis::TYPE_T> &naty,
	ibis::table::stringList &cols)
{
	cols.clear();
	char * pch;
	pch = strtok (sel, " ,.-");
	while (pch != NULL) {
		if (naty.count(pch) > 0)
			cols.push_back(pch);
		pch = strtok (NULL, " ,.-");
	}
}

void concatenate_column(ibis::table::bufferList& tbuff, ibis::table::typeList &ttypes, size_t col)
{

	tbuff[col] = ibis::table::allocateBuffer(ttypes[col],0);
	for(size_t i=0; i < part_results.size(); i++) {
		switch(ttypes[col]) {
			case ibis::BYTE:
			{
				ibis::array_t<signed char>* b1 = static_cast<ibis::array_t<signed char>*>(tbuff[col]);
				ibis::array_t<signed char>* b2 = static_cast<ibis::array_t<signed char>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::UBYTE:
			{
				ibis::array_t<unsigned char>* b1 = static_cast<ibis::array_t<unsigned char>*>(tbuff[col]);
				ibis::array_t<unsigned char>* b2 = static_cast<ibis::array_t<unsigned char>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::SHORT:
			{
				ibis::array_t<int16_t>* b1 = static_cast<ibis::array_t<int16_t>*>(tbuff[col]);
				ibis::array_t<int16_t>* b2 = static_cast<ibis::array_t<int16_t>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::USHORT:
			{
				ibis::array_t<uint16_t>* b1 = static_cast<ibis::array_t<uint16_t>*>(tbuff[col]);
				ibis::array_t<uint16_t>* b2 = static_cast<ibis::array_t<uint16_t>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::INT:
			{
				ibis::array_t<int32_t>* b1 = static_cast<ibis::array_t<int32_t>*>(tbuff[col]);
				ibis::array_t<int32_t>* b2 = static_cast<ibis::array_t<int32_t>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::UINT:
			{
				ibis::array_t<uint32_t>* b1 = static_cast<ibis::array_t<uint32_t>*>(tbuff[col]);
				ibis::array_t<uint32_t>* b2 = static_cast<ibis::array_t<uint32_t>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::LONG:
			{
				ibis::array_t<int64_t>* b1 = static_cast<ibis::array_t<int64_t>*>(tbuff[col]);
				ibis::array_t<int64_t>* b2 = static_cast<ibis::array_t<int64_t>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::ULONG:
			{
				ibis::array_t<uint64_t>* b1 = static_cast<ibis::array_t<uint64_t>*>(tbuff[col]);
				ibis::array_t<uint64_t>* b2 = static_cast<ibis::array_t<uint64_t>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::FLOAT:
			{
				ibis::array_t<float>* b1 = static_cast<ibis::array_t<float>*>(tbuff[col]);
				ibis::array_t<float>* b2 = static_cast<ibis::array_t<float>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::DOUBLE:
			{
				ibis::array_t<double>* b1 = static_cast<ibis::array_t<double>*>(tbuff[col]);
				ibis::array_t<double>* b2 = static_cast<ibis::array_t<double>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			case ibis::TEXT:
			case ibis::CATEGORY:
			{
				std::vector<std::string>* b1 = static_cast<std::vector<std::string>*>(tbuff[col]);
				std::vector<std::string>* b2 = static_cast<std::vector<std::string>*>((*part_results[i])[col]);
				b1->insert(b1->end(),b2->begin(),b2->end());
				break;
			}
			default:
				break;
		}
	}	
}

ibis::table* concatenate_results()
{
	size_t ncols = Acols.size() + Bcols.size() + 2;
	ibis::table::bufferList tbuff(ncols,0);
	ibis::table::typeList ttypes(ncols);
	ibis::table::stringList colnames(ncols);
	IBIS_BLOCK_GUARD(ibis::table::freeBuffers, ibis::util::ref(tbuff), ibis::util::ref(ttypes));

	uint32_t nrows=0;
	for(size_t i=0; i < part_results.size(); i++) {
		ibis::array_t<int32_t>* buff = static_cast<ibis::array_t<int32_t>*>((*part_results[i])[0]);
		nrows += buff->size();
	}
	
	ttypes[0] = ibis::INT;
	ttypes[1] = ibis::INT;
	boost::thread_group g;
	if (parallelize > 0) {
		g.create_thread(boost::bind(concatenate_column, tbuff, ttypes, 0));
		g.create_thread(boost::bind(concatenate_column, tbuff, ttypes, 1));
	} else {
		concatenate_column(tbuff,ttypes,0);
		concatenate_column(tbuff,ttypes,1);
	}
	colnames[0] = "start";
	colnames[1] = "end";
	size_t j=2;
	for(size_t i=0; i<Acols.size(); i++) {
		ttypes[j] = Anaty.find(Acols[i])->second;		
		if (parallelize > 0)
			g.create_thread(boost::bind(concatenate_column, tbuff, ttypes,j));
		else
			concatenate_column(tbuff,ttypes,j);
		std::string cname= "A.";
		cname.append(Acols[i]);
		colnames[j] = cname.c_str();
		j++;
	}
	for(size_t i=0; i<Bcols.size(); i++) {
		ttypes[j] = Bnaty.find(Bcols[i])->second;
		if (parallelize > 0)
			g.create_thread(boost::bind(concatenate_column, tbuff, ttypes,j));
		else
			concatenate_column(tbuff,ttypes,j);
		std::string cname= "B.";
		cname.append(Bcols[i]);
		colnames[j] = cname.c_str();
		j++;
	}
	if (parallelize > 0)
		g.join_all();
	// create the table now that it's filled
    return new ibis::bord("joined", "joined tables", nrows, tbuff, ttypes, colnames);
}

int main(int argc, char** argv)
{
    parse_args(argc, argv);

	// parse qExpr once so it can be reused in each partition
	ibis::whereClause Awhere = ibis::whereClause(Aqcnd);
	Acond = Awhere.getExpr();
	ibis::whereClause Bwhere = ibis::whereClause(Bqcnd);
	Bcond = Bwhere.getExpr();

	// populate column name => type map
	ibis::table* A = ibis::table::create(Afrom);
	Anames = A->columnNames();
	Atypes = A->columnTypes();
	Acols = A->columnNames();
    for (size_t i = 0; i < Anames.size(); ++ i) {
		Anaty.insert(std::pair<const char*,ibis::TYPE_T>(Anames[i],Atypes[i]));
	}
	ibis::table* B = ibis::table::create(Bfrom);
	Bnames = B->columnNames();
	Btypes = B->columnTypes();
	Bcols = B->columnNames();
    for (size_t i = 0; i < Bnames.size(); ++ i) {
		Bnaty.insert(std::pair<const char*,ibis::TYPE_T>(Bnames[i],Btypes[i]));
	}

	// parse Asel and Bsel columns
	if (Asel != 0 && *Asel != 0)
		fillColumnLists(Asel,Anaty,Acols);
	if (Bsel != 0 && *Bsel != 0)
		fillColumnLists(Bsel,Bnaty,Bcols);

	std::vector<const ibis::part*> Bparts;
	B->getPartitions(Bparts);
	std::map<const char*,const ibis::part*> Bpartmap;	
	for(size_t i=0; i<Bparts.size(); ++ i)
		Bpartmap.insert(std::pair<const char*,const ibis::part*>(Bparts[i]->getMetaTag("FBchr"),Bparts[i]));
	
	std::vector<const ibis::part*> Aparts;
	A->getPartitions(Aparts);
	boost::thread_group g;
	for(size_t i=0; i<Aparts.size(); ++ i) {
		const char* chr = Aparts[i]->getMetaTag("FBchr");
		if (Bpartmap.count(chr)>0) {
			if (parallelize > 0)
				g.create_thread(boost::bind(setupStacker, Aparts[i], Bpartmap.find(chr)->second));
			else
				setupStacker(Aparts[i],Bpartmap.find(chr)->second);
		}
	}
	if (parallelize > 0)
		g.join_all();

	// concatenate each part into one table?
	ibis::table* res = concatenate_results();
	std::cerr << "back from concatenate_results() res->nRows() = " << res->nRows() << std::endl;

	res->orderby("start, end");
	res->dump(std::cout);

	// write the table out - user supplied outdir
	// create one subdirectory partition with FBchr = stacked
	delete res;
	delete A;
	delete B;
    return 0;
}
