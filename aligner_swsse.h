/*
 * aligner_sse.h
 */

#ifndef ALIGNER_SWSSE_H_
#define ALIGNER_SWSSE_H_
#ifndef NO_SSE

#include "ds.h"
#include "mem_ids.h"
#include "random_source.h"
#include "scoring.h"
#include <emmintrin.h>
#include <strings.h>

/**
 * Encapsulates matrix information calculated by the SSE aligner.
 *
 * Matrix memory is laid out as follows:
 *
 * - Elements (individual cell scores) are packed into __m128i vectors
 * - Vectors are packed into quartets, quartet elements correspond to: a vector
 *   from E, one from F, one from H, and one that's "reserved"
 * - Quartets are packed into columns, where the number of quartets is
 *   determined by the number of query characters divided by the number of
 *   elements per vector
 *
 * Regarding the "reserved" element of the vector quartet: we use it for two
 * things.  First, we use the first column of reserved vectors to stage the
 * initial column of H vectors.  Second, we use the "reserved" vectors during
 * the backtrace procedure to store information about (a) which cells have been
 * traversed, (b) whether the cell is "terminal" (in local mode), etc.
 */
class SSEMatrix {

public:

	// Each matrix element is a quartet of vectors.  These constants are used
	// to identify members of the quartet.
	const static size_t E   = 0;
	const static size_t F   = 1;
	const static size_t H   = 2;
	const static size_t TMP = 3;

	SSEMatrix(int cat = 0) : nvecPerCell_(4), buf_(cat) { }

	/**
	 * Return a pointer to the matrix buffer.
	 */
	inline __m128i *ptr() {
		assert(inited_);
		return bufal_;
	}
	
	/**
	 * Return a pointer to the E vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* evec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + E;
	}

	/**
	 * Like evec, but it's allowed to ask for a pointer to one column after the
	 * final one.
	 */
	inline __m128i* evecUnsafe(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_leq(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + E;
	}

	/**
	 * Return a pointer to the F vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* fvec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + F;
	}

	/**
	 * Return a pointer to the H vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* hvec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + H;
	}

	/**
	 * Return a pointer to the TMP vector at the given row and column.  Note:
	 * here row refers to rows of vectors, not rows of elements.
	 */
	inline __m128i* tmpvec(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_lt(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + TMP;
	}

	/**
	 * Like tmpvec, but it's allowed to ask for a pointer to one column after
	 * the final one.
	 */
	inline __m128i* tmpvecUnsafe(size_t row, size_t col) {
		assert_lt(row, nvecrow_);
		assert_leq(col, nveccol_);
		return ptr() + row * rowstride() + col * colstride() + TMP;
	}
	
	/**
	 * Given a number of rows (nrow), a number of columns (ncol), and the
	 * number of words to fit inside a single __m128i vector, initialize the
	 * matrix buffer to accomodate the needed configuration of vectors.
	 */
	void init(
		size_t nrow,
		size_t ncol,
		size_t wperv)
	{
		nrow_ = nrow;
		ncol_ = ncol;
		wperv_ = wperv;
		nvecPerCol_ = (nrow + (wperv-1)) / wperv;
		// The +1 is so that we don't have to special-case the final column;
		// instead, we just write off the end of the useful part of the table
		// with pvEStore.
		buf_.resize((ncol+1) * nvecPerCell_ * nvecPerCol_ + 16);
		//bzero(buf_.ptr(), sizeof(__m128i) * ((ncol+1) * nvecPerCell_ * nvecPerCol_ + 16));
		// Get a 16-byte aligned pointer toward the beginning of the buffer.
		size_t aligned = ((size_t)buf_.ptr() + 15) & ~(0x0f);
		// Set up pointers into the buffer for fw query
		bufal_ = reinterpret_cast<__m128i*>(aligned);
		assert(wperv_ == 8 || wperv_ == 16);
		vecshift_ = (wperv_ == 8) ? 3 : 4;
		nvecrow_ = (nrow + (wperv_-1)) >> vecshift_;
		nveccol_ = ncol;
		colstride_ = nvecPerCol_ * nvecPerCell_;
		rowstride_ = nvecPerCell_;
		inited_ = true;
	}
	
	/**
	 * Return the number of __m128i's you need to skip over to get from one
	 * cell to the cell one column over from it.
	 */
	inline size_t colstride() const { return colstride_; }

	/**
	 * Return the number of __m128i's you need to skip over to get from one
	 * cell to the cell one row down from it.
	 */
	inline size_t rowstride() const { return rowstride_; }

	/**
	 * Given a row, col and matrix (i.e. E, F or H), return the corresponding
	 * element.
	 */
	int eltSlow(size_t row, size_t col, size_t mat) const;
	
	/**
	 * Given a row, col and matrix (i.e. E, F or H), return the corresponding
	 * element.
	 */
	inline int elt(size_t row, size_t col, size_t mat) const {
		assert(inited_);
		assert_lt(row, nrow_);
		assert_lt(col, ncol_);
		assert_lt(mat, 3);
		// Move to beginning of column/row
		size_t rowelt = row / nvecrow_;
		size_t rowvec = row % nvecrow_;
		size_t eltvec = (col * colstride_) + (rowvec * rowstride_) + mat;
		if(wperv_ == 16) {
			return (int)((uint8_t*)&bufal_[eltvec])[rowelt];
		} else {
			assert_eq(8, wperv_);
			return (int)((int16_t*)&bufal_[eltvec])[rowelt];
		}
	}

	/**
	 * Given a row, col and matrix (i.e. E, F or H), return the corresponding
	 * element.
	 */
	inline void* eltptr(size_t row, size_t col, size_t mat) const {
		assert(inited_);
		assert_lt(row, nrow_);
		assert_lt(col, ncol_);
		assert_lt(mat, 3);
		// Move to beginning of column/row
		size_t rowelt = row / nvecrow_;
		size_t rowvec = row % nvecrow_;
		size_t eltvec = (col * colstride_) + (rowvec * rowstride_) + mat;
		return &bufal_[eltvec] + rowelt;
	}
	
	/**
	 * Return the element in the E matrix at element row, col.
	 */
	inline int eelt(size_t row, size_t col) const {
		return elt(row, col, E);
	}

	/**
	 * Return the element in the F matrix at element row, col.
	 */
	inline int felt(size_t row, size_t col) const {
		return elt(row, col, F);
	}

	/**
	 * Return the element in the H matrix at element row, col.
	 */
	inline int helt(size_t row, size_t col) const {
		return elt(row, col, H);
	}
	
	/**
	 * Return true iff the given cell has its reportedThru bit set.
	 */
	inline bool reportedThrough(
		size_t row,          // current row
		size_t col) const    // current column
	{
		return ((masks_[row * ncol_ + col] & (1 << 0)) != 0);
	}

	/**
	 * Set the given cell's reportedThru bit.
	 */
	inline void setReportedThrough(
		size_t row,          // current row
		size_t col)          // current column
	{
		masks_[row * ncol_ + col] |= (1 << 0);
	}

	/**
	 * Return true iff the H mask has been set with a previous call to hMaskSet.
	 */
	inline bool isHMaskSet(
		size_t row,          // current row
		size_t col) const;   // current column

	/**
	 * Set the given cell's H mask.  This is the mask of remaining legal ways to
	 * backtrack from the H cell at this coordinate.  It's 5 bits long and has
	 * offset=2 into the 16-bit field.
	 */
	inline void hMaskSet(
		size_t row,          // current row
		size_t col,          // current column
		int mask);

	/**
	 * Return true iff the E mask has been set with a previous call to eMaskSet.
	 */
	inline bool isEMaskSet(
		size_t row,          // current row
		size_t col) const;   // current column

	/**
	 * Set the given cell's E mask.  This is the mask of remaining legal ways to
	 * backtrack from the E cell at this coordinate.  It's 2 bits long and has
	 * offset=8 into the 16-bit field.
	 */
	inline void eMaskSet(
		size_t row,          // current row
		size_t col,          // current column
		int mask);
	
	/**
	 * Return true iff the F mask has been set with a previous call to fMaskSet.
	 */
	inline bool isFMaskSet(
		size_t row,          // current row
		size_t col) const;   // current column

	/**
	 * Set the given cell's F mask.  This is the mask of remaining legal ways to
	 * backtrack from the F cell at this coordinate.  It's 2 bits long and has
	 * offset=11 into the 16-bit field.
	 */
	inline void fMaskSet(
		size_t row,          // current row
		size_t col,          // current column
		int mask);

	/**
	 *
	 */
	void analyzeCell(
		size_t row,          // current row
		size_t col,          // current column
		size_t ct,           // current cell type: E/F/H
		int refc,
		int readc,
		int readq,
		const Scoring& sc,   // scoring scheme
		TAlScore offsetsc,   // offset to add to each score
		TAlScore floorsc,    // local-alignment score floor
		RandomSource& rand,  // rand gen for choosing among equal options
		bool& empty,         // out: =true iff no way to backtrace
		int& cur,            // out: =type of transition
		bool& branch,        // out: =true iff we chose among >1 options
		bool& canMoveThru,   // out: =true iff ...
		bool& reportedThru); // out: =true iff ...

	/**
	 * Initialize the matrix of masks and backtracking flags.
	 */
	void initMasks() {
		masks_.resize(nrow_ * ncol_);
		bzero(masks_.ptr(), sizeof(uint16_t) * nrow_ * ncol_);
	}

	/**
	 * Return the number of rows in the dynamic programming matrix.
	 */
	size_t nrow() const {
		return nrow_;
	}

	/**
	 * Return the number of columns in the dynamic programming matrix.
	 */
	size_t ncol() const {
		return ncol_;
	}

protected:

	bool             inited_;      // initialized?
	size_t           nrow_;        // # rows
	size_t           ncol_;        // # columns
	size_t           nvecrow_;     // # vector rows (<= nrow_)
	size_t           nveccol_;     // # vector columns (<= ncol_)
	size_t           wperv_;       // # words per vector
	size_t           vecshift_;    // # bits to shift to divide by words per vec
	size_t           nvecPerCol_;  // # vectors per column
	size_t           nvecPerCell_; // # vectors per matrix cell (4)
	size_t           colstride_;   // # vectors b/t adjacent cells in same row
	size_t           rowstride_;   // # vectors b/t adjacent cells in same col
	EList<__m128i>   buf_;         // buffer for holding vectors
	EList<uint16_t>  masks_;       // buffer for masks/backtracking flags
	__m128i         *bufal_;       // 16-byte aligned version of the ptr for buf_
};

/**
 * All the data associated with the query profile and other data needed for SSE
 * alignment of a query.
 */
struct SSEData {
	SSEData(int cat = 0) : buf_(cat), mat_(cat) { }
	EList<uint8_t> buf_;         // buffer for query profile & temp vecs
	__m128i       *qprof_;       // query profile
	size_t         qprofStride_; // stride for query profile
	size_t         gbarStride_;  // gap barrier for query profile
	SSEMatrix      mat_;         // SSE matrix for holding all E, F, H vectors
	size_t         maxPen_;      // biggest penalty of all
	size_t         maxBonus_;    // biggest bonus of all
	size_t         lastIter_;    // which 128-bit striped word has final row?
	size_t         lastWord_;    // which word within 128-word has final row?
	int            bias_;        // all scores shifted up by this for unsigned
};

#endif /*ndef NO_SSE*/
#endif /*ndef ALIGNER_SWSSE_H_*/
