/*
** 2004 May 26
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** This file contains code use to manipulate "Mem" structure.  A "Mem"
** stores a single value in the VDBE.  Mem is an opaque structure visible
** only within the VDBE.  Interface routines refer to a Mem using the
** name sqlite_value
**
** $Id: vdbemem.c,v 1.141 2009/04/21 09:02:47 danielk1977 Exp $
*/
#include "sqliteInt.h"
#include "vdbeInt.h"

/*
** Call sqlite3VdbeMemExpandBlob() on the supplied value (type Mem*)
** P if required.
*/
#define expandBlob(P) (((P)->flags&MEM_Zero)?sqlite3VdbeMemExpandBlob(P):0)

/*
** If pMem is an object with a valid string representation, this routine
** ensures the internal encoding for the string representation is
** 'desiredEnc', one of SQLITE_UTF8, SQLITE_UTF16LE or SQLITE_UTF16BE.
**
** If pMem is not a string object, or the encoding of the string
** representation is already stored using the requested encoding, then this
** routine is a no-op.
**
** SQLITE_OK is returned if the conversion is successful (or not required).
** SQLITE_NOMEM may be returned if a malloc() fails during conversion
** between formats.
*/
int sqlite3VdbeChangeEncoding(Mem *pMem, int desiredEnc){
  int rc;
  assert( (pMem->flags&MEM_RowSet)==0 );
  assert( desiredEnc==SQLITE_UTF8 || desiredEnc==SQLITE_UTF16LE
           || desiredEnc==SQLITE_UTF16BE );
  if( !(pMem->flags&MEM_Str) || pMem->enc==desiredEnc ){
    return SQLITE_OK;
  }
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
#ifdef SQLITE_OMIT_UTF16
  return SQLITE_ERROR;
#else

  /* MemTranslate() may return SQLITE_OK or SQLITE_NOMEM. If NOMEM is returned,
  ** then the encoding of the value may not have changed.
  */
  rc = sqlite3VdbeMemTranslate(pMem, (u8)desiredEnc);
  assert(rc==SQLITE_OK    || rc==SQLITE_NOMEM);
  assert(rc==SQLITE_OK    || pMem->enc!=desiredEnc);
  assert(rc==SQLITE_NOMEM || pMem->enc==desiredEnc);
  return rc;
#endif
}

/*
** Make sure pMem->z points to a writable allocation of at least 
** n bytes.
**
** If the memory cell currently contains string or blob data
** and the third argument passed to this function is true, the 
** current content of the cell is preserved. Otherwise, it may
** be discarded.  
**
** This function sets the MEM_Dyn flag and clears any xDel callback.
** It also clears MEM_Ephem and MEM_Static. If the preserve flag is 
** not set, Mem.n is zeroed.
*/
int sqlite3VdbeMemGrow(Mem *pMem, int n, int preserve){
  assert( 1 >=
    ((pMem->zMalloc && pMem->zMalloc==pMem->z) ? 1 : 0) +
    (((pMem->flags&MEM_Dyn)&&pMem->xDel) ? 1 : 0) + 
    ((pMem->flags&MEM_Ephem) ? 1 : 0) + 
    ((pMem->flags&MEM_Static) ? 1 : 0)
  );
  assert( (pMem->flags&MEM_RowSet)==0 );

  if( n<32 ) n = 32;
  if( sqlite3DbMallocSize(pMem->db, pMem->zMalloc)<n ){
    if( preserve && pMem->z==pMem->zMalloc ){
      pMem->z = pMem->zMalloc = sqlite3DbReallocOrFree(pMem->db, pMem->z, n);
      preserve = 0;
    }else{
      sqlite3DbFree(pMem->db, pMem->zMalloc);
      pMem->zMalloc = sqlite3DbMallocRaw(pMem->db, n);
    }
  }

  if( preserve && pMem->z && pMem->zMalloc && pMem->z!=pMem->zMalloc ){
    memcpy(pMem->zMalloc, pMem->z, pMem->n);
  }
  if( pMem->flags&MEM_Dyn && pMem->xDel ){
    pMem->xDel((void *)(pMem->z));
  }

  pMem->z = pMem->zMalloc;
  if( pMem->z==0 ){
    pMem->flags = MEM_Null;
  }else{
    pMem->flags &= ~(MEM_Ephem|MEM_Static);
  }
  pMem->xDel = 0;
  return (pMem->z ? SQLITE_OK : SQLITE_NOMEM);
}

/*
** Make the given Mem object MEM_Dyn.  In other words, make it so
** that any TEXT or BLOB content is stored in memory obtained from
** malloc().  In this way, we know that the memory is safe to be
** overwritten or altered.
**
** Return SQLITE_OK on success or SQLITE_NOMEM if malloc fails.
*/
int sqlite3VdbeMemMakeWriteable(Mem *pMem){
  int f;
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags&MEM_RowSet)==0 );
  expandBlob(pMem);
  f = pMem->flags;
  if( (f&(MEM_Str|MEM_Blob)) && pMem->z!=pMem->zMalloc ){
    if( sqlite3VdbeMemGrow(pMem, pMem->n + 2, 1) ){
      return SQLITE_NOMEM;
    }
    pMem->z[pMem->n] = 0;
    pMem->z[pMem->n+1] = 0;
    pMem->flags |= MEM_Term;
  }

  return SQLITE_OK;
}

/*
** If the given Mem* has a zero-filled tail, turn it into an ordinary
** blob stored in dynamically allocated space.
*/
#ifndef SQLITE_OMIT_INCRBLOB
int sqlite3VdbeMemExpandBlob(Mem *pMem){
  if( pMem->flags & MEM_Zero ){
    int nByte;
    assert( pMem->flags&MEM_Blob );
    assert( (pMem->flags&MEM_RowSet)==0 );
    assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );

    /* Set nByte to the number of bytes required to store the expanded blob. */
    nByte = pMem->n + pMem->u.nZero;
    if( nByte<=0 ){
      nByte = 1;
    }
    if( sqlite3VdbeMemGrow(pMem, nByte, 1) ){
      return SQLITE_NOMEM;
    }

    memset(&pMem->z[pMem->n], 0, pMem->u.nZero);
    pMem->n += pMem->u.nZero;
    pMem->flags &= ~(MEM_Zero|MEM_Term);
  }
  return SQLITE_OK;
}
#endif


/*
** Make sure the given Mem is \u0000 terminated.
*/
int sqlite3VdbeMemNulTerminate(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  if( (pMem->flags & MEM_Term)!=0 || (pMem->flags & MEM_Str)==0 ){
    return SQLITE_OK;   /* Nothing to do */
  }
  if( sqlite3VdbeMemGrow(pMem, pMem->n+2, 1) ){
    return SQLITE_NOMEM;
  }
  pMem->z[pMem->n] = 0;
  pMem->z[pMem->n+1] = 0;
  pMem->flags |= MEM_Term;
  return SQLITE_OK;
}

/*
** Add MEM_Str to the set of representations for the given Mem.  Numbers
** are converted using sqlite3_snprintf().  Converting a BLOB to a string
** is a no-op.
**
** Existing representations MEM_Int and MEM_Real are *not* invalidated.
**
** A MEM_Null value will never be passed to this function. This function is
** used for converting values to text for returning to the user (i.e. via
** sqlite3_value_text()), or for ensuring that values to be used as btree
** keys are strings. In the former case a NULL pointer is returned the
** user and the later is an internal programming error.
*/
int sqlite3VdbeMemStringify(Mem *pMem, int enc){
  int rc = SQLITE_OK;
  int fg = pMem->flags;
  const int nByte = 32;

  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( !(fg&MEM_Zero) );
  assert( !(fg&(MEM_Str|MEM_Blob)) );
  assert( fg&(MEM_Int|MEM_Real) );
  assert( (pMem->flags&MEM_RowSet)==0 );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );


  if( sqlite3VdbeMemGrow(pMem, nByte, 0) ){
    return SQLITE_NOMEM;
  }

  /* For a Real or Integer, use sqlite3_mprintf() to produce the UTF-8
  ** string representation of the value. Then, if the required encoding
  ** is UTF-16le or UTF-16be do a translation.
  ** 
  ** FIX ME: It would be better if sqlite3_snprintf() could do UTF-16.
  */
  if( fg & MEM_Int ){
    sqlite3_snprintf(nByte, pMem->z, "%lld", pMem->u.i);
  }else{
    assert( fg & MEM_Real );
    sqlite3_snprintf(nByte, pMem->z, "%!.15g", pMem->r);
  }
  pMem->n = sqlite3Strlen30(pMem->z);
  pMem->enc = SQLITE_UTF8;
  pMem->flags |= MEM_Str|MEM_Term;
  sqlite3VdbeChangeEncoding(pMem, enc);
  return rc;
}

/*
** Memory cell pMem contains the context of an aggregate function.
** This routine calls the finalize method for that function.  The
** result of the aggregate is stored back into pMem.
**
** Return SQLITE_ERROR if the finalizer reports an error.  SQLITE_OK
** otherwise.
*/
int sqlite3VdbeMemFinalize(Mem *pMem, FuncDef *pFunc){
  int rc = SQLITE_OK;
  if( pFunc && pFunc->xFinalize ){
    sqlite3_context ctx;
    assert( (pMem->flags & MEM_Null)!=0 || pFunc==pMem->u.pDef );
    assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
    memset(&ctx, 0, sizeof(ctx));
    ctx.s.flags = MEM_Null;
    ctx.s.db = pMem->db;
    ctx.pMem = pMem;
    ctx.pFunc = pFunc;
    pFunc->xFinalize(&ctx);
    assert( 0==(pMem->flags&MEM_Dyn) && !pMem->xDel );
    sqlite3DbFree(pMem->db, pMem->zMalloc);
    memcpy(pMem, &ctx.s, sizeof(ctx.s));
    rc = (ctx.isError?SQLITE_ERROR:SQLITE_OK);
  }
  return rc;
}

/*
** If the memory cell contains a string value that must be freed by
** invoking an external callback, free it now. Calling this function
** does not free any Mem.zMalloc buffer.
*/
void sqlite3VdbeMemReleaseExternal(Mem *p){
  assert( p->db==0 || sqlite3_mutex_held(p->db->mutex) );
  if( p->flags&(MEM_Agg|MEM_Dyn|MEM_RowSet|MEM_RowHash) ){
    if( p->flags&MEM_Agg ){
      sqlite3VdbeMemFinalize(p, p->u.pDef);
      assert( (p->flags & MEM_Agg)==0 );
      sqlite3VdbeMemRelease(p);
    }else if( p->flags&MEM_Dyn && p->xDel ){
      assert( (p->flags&MEM_RowSet)==0 );
      p->xDel((void *)p->z);
      p->xDel = 0;
    }else if( p->flags&MEM_RowSet ){
      sqlite3RowSetClear(p->u.pRowSet);
    }else if( p->flags&MEM_RowHash ){
      sqlite3RowhashDestroy(p->u.pRowHash);
      p->u.pRowHash = 0;
    }
  }
}

/*
** Release any memory held by the Mem. This may leave the Mem in an
** inconsistent state, for example with (Mem.z==0) and
** (Mem.type==SQLITE_TEXT).
*/
void sqlite3VdbeMemRelease(Mem *p){
  sqlite3VdbeMemReleaseExternal(p);
  sqlite3DbFree(p->db, p->zMalloc);
  p->z = 0;
  p->zMalloc = 0;
  p->xDel = 0;
}

/*
** Convert a 64-bit IEEE double into a 64-bit signed integer.
** If the double is too large, return 0x8000000000000000.
**
** Most systems appear to do this simply by assigning
** variables and without the extra range tests.  But
** there are reports that windows throws an expection
** if the floating point value is out of range. (See ticket #2880.)
** Because we do not completely understand the problem, we will
** take the conservative approach and always do range tests
** before attempting the conversion.
*/
static i64 doubleToInt64(double r){
  /*
  ** Many compilers we encounter do not define constants for the
  ** minimum and maximum 64-bit integers, or they define them
  ** inconsistently.  And many do not understand the "LL" notation.
  ** So we define our own static constants here using nothing
  ** larger than a 32-bit integer constant.
  */
  static const i64 maxInt = LARGEST_INT64;
  static const i64 minInt = SMALLEST_INT64;

  if( r<(double)minInt ){
    return minInt;
  }else if( r>(double)maxInt ){
    /* minInt is correct here - not maxInt.  It turns out that assigning
    ** a very large positive number to an integer results in a very large
    ** negative integer.  This makes no sense, but it is what x86 hardware
    ** does so for compatibility we will do the same in software. */
    return minInt;
  }else{
    return (i64)r;
  }
}

/*
** Return some kind of integer value which is the best we can do
** at representing the value that *pMem describes as an integer.
** If pMem is an integer, then the value is exact.  If pMem is
** a floating-point then the value returned is the integer part.
** If pMem is a string or blob, then we make an attempt to convert
** it into a integer and return that.  If pMem represents an
** an SQL-NULL value, return 0.
**
** If pMem represents a string value, its encoding might be changed.
*/
i64 sqlite3VdbeIntValue(Mem *pMem){
  int flags;
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );
  flags = pMem->flags;
  if( flags & MEM_Int ){
    return pMem->u.i;
  }else if( flags & MEM_Real ){
    return doubleToInt64(pMem->r);
  }else if( flags & (MEM_Str|MEM_Blob) ){
    i64 value;
    pMem->flags |= MEM_Str;
    if( sqlite3VdbeChangeEncoding(pMem, SQLITE_UTF8)
       || sqlite3VdbeMemNulTerminate(pMem) ){
      return 0;
    }
    assert( pMem->z );
    sqlite3Atoi64(pMem->z, &value);
    return value;
  }else{
    return 0;
  }
}

/*
** Return the best representation of pMem that we can get into a
** double.  If pMem is already a double or an integer, return its
** value.  If it is a string or blob, try to convert it to a double.
** If it is a NULL, return 0.0.
*/
double sqlite3VdbeRealValue(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );
  if( pMem->flags & MEM_Real ){
    return pMem->r;
  }else if( pMem->flags & MEM_Int ){
    return (double)pMem->u.i;
  }else if( pMem->flags & (MEM_Str|MEM_Blob) ){
    /* (double)0 In case of SQLITE_OMIT_FLOATING_POINT... */
    double val = (double)0;
    pMem->flags |= MEM_Str;
    if( sqlite3VdbeChangeEncoding(pMem, SQLITE_UTF8)
       || sqlite3VdbeMemNulTerminate(pMem) ){
      /* (double)0 In case of SQLITE_OMIT_FLOATING_POINT... */
      return (double)0;
    }
    assert( pMem->z );
    sqlite3AtoF(pMem->z, &val);
    return val;
  }else{
    /* (double)0 In case of SQLITE_OMIT_FLOATING_POINT... */
    return (double)0;
  }
}

/*
** The MEM structure is already a MEM_Real.  Try to also make it a
** MEM_Int if we can.
*/
void sqlite3VdbeIntegerAffinity(Mem *pMem){
  assert( pMem->flags & MEM_Real );
  assert( (pMem->flags & MEM_RowSet)==0 );
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->u.i = doubleToInt64(pMem->r);
  if( pMem->r==(double)pMem->u.i ){
    pMem->flags |= MEM_Int;
  }
}

/*
** Convert pMem to type integer.  Invalidate any prior representations.
*/
int sqlite3VdbeMemIntegerify(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags & MEM_RowSet)==0 );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->u.i = sqlite3VdbeIntValue(pMem);
  MemSetTypeFlag(pMem, MEM_Int);
  return SQLITE_OK;
}

/*
** Convert pMem so that it is of type MEM_Real.
** Invalidate any prior representations.
*/
int sqlite3VdbeMemRealify(Mem *pMem){
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( EIGHT_BYTE_ALIGNMENT(pMem) );

  pMem->r = sqlite3VdbeRealValue(pMem);
  MemSetTypeFlag(pMem, MEM_Real);
  return SQLITE_OK;
}

/*
** Convert pMem so that it has types MEM_Real or MEM_Int or both.
** Invalidate any prior representations.
*/
int sqlite3VdbeMemNumerify(Mem *pMem){
  double r1, r2;
  i64 i;
  assert( (pMem->flags & (MEM_Int|MEM_Real|MEM_Null))==0 );
  assert( (pMem->flags & (MEM_Blob|MEM_Str))!=0 );
  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  r1 = sqlite3VdbeRealValue(pMem);
  i = doubleToInt64(r1);
  r2 = (double)i;
  if( r1==r2 ){
    sqlite3VdbeMemIntegerify(pMem);
  }else{
    pMem->r = r1;
    MemSetTypeFlag(pMem, MEM_Real);
  }
  return SQLITE_OK;
}

/*
** Delete any previous value and set the value stored in *pMem to NULL.
*/
void sqlite3VdbeMemSetNull(Mem *pMem){
  if( pMem->flags & MEM_RowSet ){
    sqlite3RowSetClear(pMem->u.pRowSet);
  }
  MemSetTypeFlag(pMem, MEM_Null);
  pMem->type = SQLITE_NULL;
}

/*
** Delete any previous value and set the value to be a BLOB of length
** n containing all zeros.
*/
void sqlite3VdbeMemSetZeroBlob(Mem *pMem, int n){
  sqlite3VdbeMemRelease(pMem);
  pMem->flags = MEM_Blob|MEM_Zero;
  pMem->type = SQLITE_BLOB;
  pMem->n = 0;
  if( n<0 ) n = 0;
  pMem->u.nZero = n;
  pMem->enc = SQLITE_UTF8;
}

/*
** Delete any previous value and set the value stored in *pMem to val,
** manifest type INTEGER.
*/
void sqlite3VdbeMemSetInt64(Mem *pMem, i64 val){
  sqlite3VdbeMemRelease(pMem);
  pMem->u.i = val;
  pMem->flags = MEM_Int;
  pMem->type = SQLITE_INTEGER;
}

/*
** Delete any previous value and set the value stored in *pMem to val,
** manifest type REAL.
*/
void sqlite3VdbeMemSetDouble(Mem *pMem, double val){
  if( sqlite3IsNaN(val) ){
    sqlite3VdbeMemSetNull(pMem);
  }else{
    sqlite3VdbeMemRelease(pMem);
    pMem->r = val;
    pMem->flags = MEM_Real;
    pMem->type = SQLITE_FLOAT;
  }
}

/*
** Delete any previous value and set the value of pMem to be an
** empty boolean index.
*/
void sqlite3VdbeMemSetRowSet(Mem *pMem){
  sqlite3 *db = pMem->db;
  assert( db!=0 );
  if( pMem->flags & MEM_RowSet ){
    sqlite3RowSetClear(pMem->u.pRowSet);
  }else{
    sqlite3VdbeMemRelease(pMem);
    pMem->zMalloc = sqlite3DbMallocRaw(db, 64);
  }
  if( db->mallocFailed ){
    pMem->flags = MEM_Null;
  }else{
    assert( pMem->zMalloc );
    pMem->u.pRowSet = sqlite3RowSetInit(db, pMem->zMalloc, 
                                       sqlite3DbMallocSize(db, pMem->zMalloc));
    assert( pMem->u.pRowSet!=0 );
    pMem->flags = MEM_RowSet;
  }
}

/*
** Return true if the Mem object contains a TEXT or BLOB that is
** too large - whose size exceeds SQLITE_MAX_LENGTH.
*/
int sqlite3VdbeMemTooBig(Mem *p){
  assert( p->db!=0 );
  if( p->flags & (MEM_Str|MEM_Blob) ){
    int n = p->n;
    if( p->flags & MEM_Zero ){
      n += p->u.nZero;
    }
    return n>p->db->aLimit[SQLITE_LIMIT_LENGTH];
  }
  return 0; 
}

/*
** Size of struct Mem not including the Mem.zMalloc member.
*/
#define MEMCELLSIZE (size_t)(&(((Mem *)0)->zMalloc))

/*
** Make an shallow copy of pFrom into pTo.  Prior contents of
** pTo are freed.  The pFrom->z field is not duplicated.  If
** pFrom->z is used, then pTo->z points to the same thing as pFrom->z
** and flags gets srcType (either MEM_Ephem or MEM_Static).
*/
void sqlite3VdbeMemShallowCopy(Mem *pTo, const Mem *pFrom, int srcType){
  assert( (pFrom->flags & MEM_RowSet)==0 );
  sqlite3VdbeMemReleaseExternal(pTo);
  memcpy(pTo, pFrom, MEMCELLSIZE);
  pTo->xDel = 0;
  if( (pFrom->flags&MEM_Dyn)!=0 || pFrom->z==pFrom->zMalloc ){
    pTo->flags &= ~(MEM_Dyn|MEM_Static|MEM_Ephem);
    assert( srcType==MEM_Ephem || srcType==MEM_Static );
    pTo->flags |= srcType;
  }
}

/*
** Make a full copy of pFrom into pTo.  Prior contents of pTo are
** freed before the copy is made.
*/
int sqlite3VdbeMemCopy(Mem *pTo, const Mem *pFrom){
  int rc = SQLITE_OK;

  assert( (pFrom->flags & MEM_RowSet)==0 );
  sqlite3VdbeMemReleaseExternal(pTo);
  memcpy(pTo, pFrom, MEMCELLSIZE);
  pTo->flags &= ~MEM_Dyn;

  if( pTo->flags&(MEM_Str|MEM_Blob) ){
    if( 0==(pFrom->flags&MEM_Static) ){
      pTo->flags |= MEM_Ephem;
      rc = sqlite3VdbeMemMakeWriteable(pTo);
    }
  }

  return rc;
}

/*
** Transfer the contents of pFrom to pTo. Any existing value in pTo is
** freed. If pFrom contains ephemeral data, a copy is made.
**
** pFrom contains an SQL NULL when this routine returns.
*/
void sqlite3VdbeMemMove(Mem *pTo, Mem *pFrom){
  assert( pFrom->db==0 || sqlite3_mutex_held(pFrom->db->mutex) );
  assert( pTo->db==0 || sqlite3_mutex_held(pTo->db->mutex) );
  assert( pFrom->db==0 || pTo->db==0 || pFrom->db==pTo->db );

  sqlite3VdbeMemRelease(pTo);
  memcpy(pTo, pFrom, sizeof(Mem));
  pFrom->flags = MEM_Null;
  pFrom->xDel = 0;
  pFrom->zMalloc = 0;
}

/*
** Change the value of a Mem to be a string or a BLOB.
**
** The memory management strategy depends on the value of the xDel
** parameter. If the value passed is SQLITE_TRANSIENT, then the 
** string is copied into a (possibly existing) buffer managed by the 
** Mem structure. Otherwise, any existing buffer is freed and the
** pointer copied.
*/
int sqlite3VdbeMemSetStr(
  Mem *pMem,          /* Memory cell to set to string value */
  const char *z,      /* String pointer */
  int n,              /* Bytes in string, or negative */
  u8 enc,             /* Encoding of z.  0 for BLOBs */
  void (*xDel)(void*) /* Destructor function */
){
  int nByte = n;      /* New value for pMem->n */
  int iLimit;         /* Maximum allowed string or blob size */
  u16 flags = 0;      /* New value for pMem->flags */

  assert( pMem->db==0 || sqlite3_mutex_held(pMem->db->mutex) );
  assert( (pMem->flags & MEM_RowSet)==0 );

  /* If z is a NULL pointer, set pMem to contain an SQL NULL. */
  if( !z ){
    sqlite3VdbeMemSetNull(pMem);
    return SQLITE_OK;
  }

  if( pMem->db ){
    iLimit = pMem->db->aLimit[SQLITE_LIMIT_LENGTH];
  }else{
    iLimit = SQLITE_MAX_LENGTH;
  }
  flags = (enc==0?MEM_Blob:MEM_Str);
  if( nByte<0 ){
    assert( enc!=0 );
    if( enc==SQLITE_UTF8 ){
      for(nByte=0; nByte<=iLimit && z[nByte]; nByte++){}
    }else{
      for(nByte=0; nByte<=iLimit && (z[nByte] | z[nByte+1]); nByte+=2){}
    }
    flags |= MEM_Term;
  }

  /* The following block sets the new values of Mem.z and Mem.xDel. It
  ** also sets a flag in local variable "flags" to indicate the memory
  ** management (one of MEM_Dyn or MEM_Static).
  */
  if( xDel==SQLITE_TRANSIENT ){
    int nAlloc = nByte;
    if( flags&MEM_Term ){
      nAlloc += (enc==SQLITE_UTF8?1:2);
    }
    if( nByte>iLimit ){
      return SQLITE_TOOBIG;
    }
    if( sqlite3VdbeMemGrow(pMem, nAlloc, 0) ){
      return SQLITE_NOMEM;
    }
    memcpy(pMem->z, z, nAlloc);
  }else if( xDel==SQLITE_DYNAMIC ){
    sqlite3VdbeMemRelease(pMem);
    pMem->zMalloc = pMem->z = (char *)z;
    pMem->xDel = 0;
  }else{
    sqlite3VdbeMemRelease(pMem);
    pMem->z = (char *)z;
    pMem->xDel = xDel;
    flags |= ((xDel==SQLITE_STATIC)?MEM_Static:MEM_Dyn);
  }
  if( nByte>iLimit ){
    return SQLITE_TOOBIG;
  }

  pMem->n = nByte;
  pMem->flags = flags;
  pMem->enc = (enc==0 ? SQLITE_UTF8 : enc);
  pMem->type = (enc==0 ? SQLITE_BLOB : SQLITE_TEXT);

#ifndef SQLITE_OMIT_UTF16
  if( pMem->enc!=SQLITE_UTF8 && sqlite3VdbeMemHandleBom(pMem) ){
    return SQLITE_NOMEM;
  }
#endif

  return SQLITE_OK;
}

/*
** Compare the values contained by the two memory cells, returning
** negative, zero or positive if pMem1 is less than, equal to, or greater
** than pMem2. Sorting order is NULL's first, followed by numbers (integers
** and reals) sorted numerically, followed by text ordered by the collating
** sequence pColl and finally blob's ordered by memcmp().
**
** Two NULL values are considered equal by this function.
*/
int sqlite3MemCompare(const Mem *pMem1, const Mem *pMem2, const CollSeq *pColl){
  int rc;
  int f1, f2;
  int combined_flags;

  /* Interchange pMem1 and pMem2 if the collating sequence specifies
  ** DESC order.
  */
  f1 = pMem1->flags;
  f2 = pMem2->flags;
  combined_flags = f1|f2;
  assert( (combined_flags & MEM_RowSet)==0 );
 
  /* If one value is NULL, it is less than the other. If both values
  ** are NULL, return 0.
  */
  if( combined_flags&MEM_Null ){
    return (f2&MEM_Null) - (f1&MEM_Null);
  }

  /* If one value is a number and the other is not, the number is less.
  ** If both are numbers, compare as reals if one is a real, or as integers
  ** if both values are integers.
  */
  if( combined_flags&(MEM_Int|MEM_Real) ){
    if( !(f1&(MEM_Int|MEM_Real)) ){
      return 1;
    }
    if( !(f2&(MEM_Int|MEM_Real)) ){
      return -1;
    }
    if( (f1 & f2 & MEM_Int)==0 ){
      double r1, r2;
      if( (f1&MEM_Real)==0 ){
        r1 = (double)pMem1->u.i;
      }else{
        r1 = pMem1->r;
      }
      if( (f2&MEM_Real)==0 ){
        r2 = (double)pMem2->u.i;
      }else{
        r2 = pMem2->r;
      }
      if( r1<r2 ) return -1;
      if( r1>r2 ) return 1;
      return 0;
    }else{
      assert( f1&MEM_Int );
      assert( f2&MEM_Int );
      if( pMem1->u.i < pMem2->u.i ) return -1;
      if( pMem1->u.i > pMem2->u.i ) return 1;
      return 0;
    }
  }

  /* If one value is a string and the other is a blob, the string is less.
  ** If both are strings, compare using the collating functions.
  */
  if( combined_flags&MEM_Str ){
    if( (f1 & MEM_Str)==0 ){
      return 1;
    }
    if( (f2 & MEM_Str)==0 ){
      return -1;
    }

    assert( pMem1->enc==pMem2->enc );
    assert( pMem1->enc==SQLITE_UTF8 || 
            pMem1->enc==SQLITE_UTF16LE || pMem1->enc==SQLITE_UTF16BE );

    /* The collation sequence must be defined at this point, even if
    ** the user deletes the collation sequence after the vdbe program is
    ** compiled (this was not always the case).
    */
    assert( !pColl || pColl->xCmp );

    if( pColl ){
      if( pMem1->enc==pColl->enc ){
        /* The strings are already in the correct encoding.  Call the
        ** comparison function directly */
        return pColl->xCmp(pColl->pUser,pMem1->n,pMem1->z,pMem2->n,pMem2->z);
      }else{
        const void *v1, *v2;
        int n1, n2;
        Mem c1;
        Mem c2;
        memset(&c1, 0, sizeof(c1));
        memset(&c2, 0, sizeof(c2));
        sqlite3VdbeMemShallowCopy(&c1, pMem1, MEM_Ephem);
        sqlite3VdbeMemShallowCopy(&c2, pMem2, MEM_Ephem);
        v1 = sqlite3ValueText((sqlite3_value*)&c1, pColl->enc);
        n1 = v1==0 ? 0 : c1.n;
        v2 = sqlite3ValueText((sqlite3_value*)&c2, pColl->enc);
        n2 = v2==0 ? 0 : c2.n;
        rc = pColl->xCmp(pColl->pUser, n1, v1, n2, v2);
        sqlite3VdbeMemRelease(&c1);
        sqlite3VdbeMemRelease(&c2);
        return rc;
      }
    }
    /* If a NULL pointer was passed as the collate function, fall through
    ** to the blob case and use memcmp().  */
  }
 
  /* Both values must be blobs.  Compare using memcmp().  */
  rc = memcmp(pMem1->z, pMem2->z, (pMem1->n>pMem2->n)?pMem2->n:pMem1->n);
  if( rc==0 ){
    rc = pMem1->n - pMem2->n;
  }
  return rc;
}

/*
** Move data out of a btree key or data field and into a Mem structure.
** The data or key is taken from the entry that pCur is currently pointing
** to.  offset and amt determine what portion of the data or key to retrieve.
** key is true to get the key or false to get data.  The result is written
** into the pMem element.
**
** The pMem structure is assumed to be uninitialized.  Any prior content
** is overwritten without being freed.
**
** If this routine fails for any reason (malloc returns NULL or unable
** to read from the disk) then the pMem is left in an inconsistent state.
*/
int sqlite3VdbeMemFromBtree(
  BtCursor *pCur,   /* Cursor pointing at record to retrieve. */
  int offset,       /* Offset from the start of data to return bytes from. */
  int amt,          /* Number of bytes to return. */
  int key,          /* If true, retrieve from the btree key, not data. */
  Mem *pMem         /* OUT: Return data in this Mem structure. */
){
  char *zData;       /* Data from the btree layer */
  int available = 0; /* Number of bytes available on the local btree page */
  sqlite3 *db;       /* Database connection */
  int rc = SQLITE_OK;

  db = sqlite3BtreeCursorDb(pCur);
  assert( sqlite3_mutex_held(db->mutex) );
  assert( (pMem->flags & MEM_RowSet)==0 );
  if( key ){
    zData = (char *)sqlite3BtreeKeyFetch(pCur, &available);
  }else{
    zData = (char *)sqlite3BtreeDataFetch(pCur, &available);
  }
  assert( zData!=0 );

  if( offset+amt<=available && ((pMem->flags&MEM_Dyn)==0 || pMem->xDel) ){
    sqlite3VdbeMemRelease(pMem);
    pMem->z = &zData[offset];
    pMem->flags = MEM_Blob|MEM_Ephem;
  }else if( SQLITE_OK==(rc = sqlite3VdbeMemGrow(pMem, amt+2, 0)) ){
    pMem->flags = MEM_Blob|MEM_Dyn|MEM_Term;
    pMem->enc = 0;
    pMem->type = SQLITE_BLOB;
    if( key ){
      rc = sqlite3BtreeKey(pCur, offset, amt, pMem->z);
    }else{
      rc = sqlite3BtreeData(pCur, offset, amt, pMem->z);
    }
    pMem->z[amt] = 0;
    pMem->z[amt+1] = 0;
    if( rc!=SQLITE_OK ){
      sqlite3VdbeMemRelease(pMem);
    }
  }
  pMem->n = amt;

  return rc;
}

/* This function is only available internally, it is not part of the
** external API. It works in a similar way to sqlite3_value_text(),
** except the data returned is in the encoding specified by the second
** parameter, which must be one of SQLITE_UTF16BE, SQLITE_UTF16LE or
** SQLITE_UTF8.
**
** (2006-02-16:)  The enc value can be or-ed with SQLITE_UTF16_ALIGNED.
** If that is the case, then the result must be aligned on an even byte
** boundary.
*/
const void *sqlite3ValueText(sqlite3_value* pVal, u8 enc){
  if( !pVal ) return 0;

  assert( pVal->db==0 || sqlite3_mutex_held(pVal->db->mutex) );
  assert( (enc&3)==(enc&~SQLITE_UTF16_ALIGNED) );
  assert( (pVal->flags & MEM_RowSet)==0 );

  if( pVal->flags&MEM_Null ){
    return 0;
  }
  assert( (MEM_Blob>>3) == MEM_Str );
  pVal->flags |= (pVal->flags & MEM_Blob)>>3;
  expandBlob(pVal);
  if( pVal->flags&MEM_Str ){
    sqlite3VdbeChangeEncoding(pVal, enc & ~SQLITE_UTF16_ALIGNED);
    if( (enc & SQLITE_UTF16_ALIGNED)!=0 && 1==(1&SQLITE_PTR_TO_INT(pVal->z)) ){
      assert( (pVal->flags & (MEM_Ephem|MEM_Static))!=0 );
      if( sqlite3VdbeMemMakeWriteable(pVal)!=SQLITE_OK ){
        return 0;
      }
    }
    sqlite3VdbeMemNulTerminate(pVal);
  }else{
    assert( (pVal->flags&MEM_Blob)==0 );
    sqlite3VdbeMemStringify(pVal, enc);
    assert( 0==(1&SQLITE_PTR_TO_INT(pVal->z)) );
  }
  assert(pVal->enc==(enc & ~SQLITE_UTF16_ALIGNED) || pVal->db==0
              || pVal->db->mallocFailed );
  if( pVal->enc==(enc & ~SQLITE_UTF16_ALIGNED) ){
    return pVal->z;
  }else{
    return 0;
  }
}

/*
** Create a new sqlite3_value object.
*/
sqlite3_value *sqlite3ValueNew(sqlite3 *db){
  Mem *p = sqlite3DbMallocZero(db, sizeof(*p));
  if( p ){
    p->flags = MEM_Null;
    p->type = SQLITE_NULL;
    p->db = db;
  }
  return p;
}

/*
** Create a new sqlite3_value object, containing the value of pExpr.
**
** This only works for very simple expressions that consist of one constant
** token (i.e. "5", "5.1", "'a string'"). If the expression can
** be converted directly into a value, then the value is allocated and
** a pointer written to *ppVal. The caller is responsible for deallocating
** the value by passing it to sqlite3ValueFree() later on. If the expression
** cannot be converted to a value, then *ppVal is set to NULL.
*/
int sqlite3ValueFromExpr(
  sqlite3 *db,              /* The database connection */
  Expr *pExpr,              /* The expression to evaluate */
  u8 enc,                   /* Encoding to use */
  u8 affinity,              /* Affinity to use */
  sqlite3_value **ppVal     /* Write the new value here */
){
  int op;
  char *zVal = 0;
  sqlite3_value *pVal = 0;

  if( !pExpr ){
    *ppVal = 0;
    return SQLITE_OK;
  }
  op = pExpr->op;

  if( op==TK_STRING || op==TK_FLOAT || op==TK_INTEGER ){
    zVal = sqlite3DbStrNDup(db, (char*)pExpr->token.z, pExpr->token.n);
    pVal = sqlite3ValueNew(db);
    if( !zVal || !pVal ) goto no_mem;
    sqlite3Dequote(zVal);
    sqlite3ValueSetStr(pVal, -1, zVal, SQLITE_UTF8, SQLITE_DYNAMIC);
    if( (op==TK_INTEGER || op==TK_FLOAT ) && affinity==SQLITE_AFF_NONE ){
      sqlite3ValueApplyAffinity(pVal, SQLITE_AFF_NUMERIC, enc);
    }else{
      sqlite3ValueApplyAffinity(pVal, affinity, enc);
    }
  }else if( op==TK_UMINUS ) {
    if( SQLITE_OK==sqlite3ValueFromExpr(db,pExpr->pLeft,enc,affinity,&pVal) ){
      pVal->u.i = -1 * pVal->u.i;
      /* (double)-1 In case of SQLITE_OMIT_FLOATING_POINT... */
      pVal->r = (double)-1 * pVal->r;
    }
  }
#ifndef SQLITE_OMIT_BLOB_LITERAL
  else if( op==TK_BLOB ){
    int nVal;
    assert( pExpr->token.n>=3 );
    assert( pExpr->token.z[0]=='x' || pExpr->token.z[0]=='X' );
    assert( pExpr->token.z[1]=='\'' );
    assert( pExpr->token.z[pExpr->token.n-1]=='\'' );
    pVal = sqlite3ValueNew(db);
    if( !pVal ) goto no_mem;
    nVal = pExpr->token.n - 3;
    zVal = (char*)pExpr->token.z + 2;
    sqlite3VdbeMemSetStr(pVal, sqlite3HexToBlob(db, zVal, nVal), nVal/2,
                         0, SQLITE_DYNAMIC);
  }
#endif

  *ppVal = pVal;
  return SQLITE_OK;

no_mem:
  db->mallocFailed = 1;
  sqlite3DbFree(db, zVal);
  sqlite3ValueFree(pVal);
  *ppVal = 0;
  return SQLITE_NOMEM;
}

/*
** Change the string value of an sqlite3_value object
*/
void sqlite3ValueSetStr(
  sqlite3_value *v,     /* Value to be set */
  int n,                /* Length of string z */
  const void *z,        /* Text of the new string */
  u8 enc,               /* Encoding to use */
  void (*xDel)(void*)   /* Destructor for the string */
){
  if( v ) sqlite3VdbeMemSetStr((Mem *)v, z, n, enc, xDel);
}

/*
** Free an sqlite3_value object
*/
void sqlite3ValueFree(sqlite3_value *v){
  if( !v ) return;
  sqlite3VdbeMemRelease((Mem *)v);
  sqlite3DbFree(((Mem*)v)->db, v);
}

/*
** Return the number of bytes in the sqlite3_value object assuming
** that it uses the encoding "enc"
*/
int sqlite3ValueBytes(sqlite3_value *pVal, u8 enc){
  Mem *p = (Mem*)pVal;
  if( (p->flags & MEM_Blob)!=0 || sqlite3ValueText(pVal, enc) ){
    if( p->flags & MEM_Zero ){
      return p->n + p->u.nZero;
    }else{
      return p->n;
    }
  }
  return 0;
}
