#include <Rdefines.h>
#include "data.table.h"

SEXP coerceToRealListR(SEXP obj) {
  // accept atomic/list of integer/logical/real returns list of real
  int protecti = 0;
  SEXP x = R_NilValue;
  if (isVectorAtomic(obj)) {
    x = PROTECT(allocVector(VECSXP, 1)); protecti++;
    if (isReal(obj)) {
      SET_VECTOR_ELT(x, 0, obj);
    } else if (isInteger(obj) || isLogical(obj)) {
      SET_VECTOR_ELT(x, 0, coerceVector(obj, REALSXP));
    } else {
      error("x must be of type numeric or logical");
    }
  } else {
    R_len_t nobj = length(obj);
    x = PROTECT(allocVector(VECSXP, nobj)); protecti++;
    for (R_len_t i=0; i<nobj; i++) {
      if (isReal(VECTOR_ELT(obj, i))) {
        SET_VECTOR_ELT(x, i, VECTOR_ELT(obj, i));
      } else if (isInteger(VECTOR_ELT(obj, i)) || isLogical(VECTOR_ELT(obj, i))) {
        SET_VECTOR_ELT(x, i, coerceVector(VECTOR_ELT(obj, i), REALSXP));
      } else {
        error("x must be list, data.frame or data.table of numeric or logical types");
      }
    }
  }
  UNPROTECT(protecti);
  return x;
}

SEXP frollfunR(SEXP fun, SEXP obj, SEXP k, SEXP fill, SEXP algo, SEXP align, SEXP narm, SEXP hasna, SEXP adaptive) {
  int protecti = 0;
  const bool verbose = GetVerbose();

  if (!xlength(obj))
    return(obj);                                                // empty input: NULL, list()
  double tic = 0;
  if (verbose)
    tic = omp_get_wtime();
  SEXP x = PROTECT(coerceToRealListR(obj)); protecti++;
  R_len_t nx=length(x);                                         // number of columns to roll on

  if (xlength(k) == 0)                                          // check that window is non zero length
    error("n must be non 0 length");

  if (!isLogical(adaptive) || length(adaptive) != 1 || LOGICAL(adaptive)[0] == NA_LOGICAL)
    error("adaptive must be TRUE or FALSE");
  bool badaptive = LOGICAL(adaptive)[0];

  R_len_t nk = 0;                                               // number of rolling windows, for adaptive might be atomic to be wrapped into list, 0 for clang -Wall
  SEXP ik = R_NilValue;                                         // holds integer window width, if doing non-adaptive roll fun
  SEXP kl = R_NilValue;                                         // holds adaptive window width, if doing adaptive roll fun
  if (!badaptive) {                                             // validating n input for adaptive=FALSE
    if (isNewList(k))
      error("n must be integer, list is accepted for adaptive TRUE");

    if (isInteger(k)) {                                        // check that k is integer vector
      ik = k;
    } else if (isReal(k)) {                                    // if n is double then convert to integer
      ik = PROTECT(coerceVector(k, INTSXP)); protecti++;
    } else {
      error("n must be integer");
    }

    nk = length(k);
    R_len_t i=0;                                                // check that all window values positive
    while (i < nk && INTEGER(ik)[i] > 0) i++;
    if (i != nk)
      error("n must be positive integer values (> 0)");
  } else {                                                      // validating n input for adaptive=TRUE
    if (isVectorAtomic(k)) {                                    // if not-list then wrap into list
      kl = PROTECT(allocVector(VECSXP, 1)); protecti++;
      if (isInteger(k)) {                                       // check that k is integer vector
        SET_VECTOR_ELT(kl, 0, k);
      } else if (isReal(k)) {                                   // if n is double then convert to integer
        SET_VECTOR_ELT(kl, 0, coerceVector(k, INTSXP));
      } else {
        error("n must be integer vector or list of integer vectors");
      }
      nk = 1;
    } else {
      nk = length(k);
      kl = PROTECT(allocVector(VECSXP, nk)); protecti++;
      for (R_len_t i=0; i<nk; i++) {
        if (isInteger(VECTOR_ELT(k, i))) {
          SET_VECTOR_ELT(kl, i, VECTOR_ELT(k, i));
        } else if (isReal(VECTOR_ELT(k, i))) {                  // coerce double types to integer
          SET_VECTOR_ELT(kl, i, coerceVector(VECTOR_ELT(k, i), INTSXP));
        } else {
          error("n must be integer vector or list of integer vectors");
        }
      }
    }
  }
  int* ikl[nk];                                                 // pointers to adaptive window width
  if (badaptive) {
    for (int j=0; j<nk; j++) ikl[j] = INTEGER(VECTOR_ELT(kl, j));
  }

  if (!IS_TRUE_OR_FALSE(narm))
    error("na.rm must be TRUE or FALSE");

  if (!isLogical(hasna) || length(hasna)!=1)
    error("hasNA must be TRUE, FALSE or NA");
  if (LOGICAL(hasna)[0]==FALSE && LOGICAL(narm)[0])
    error("using hasNA FALSE and na.rm TRUE does not make sense, if you know there are NA values use hasNA TRUE, otherwise leave it as default NA");

  int ialign;                                                   // decode align to integer
  if (!strcmp(CHAR(STRING_ELT(align, 0)), "right"))
    ialign = 1;
  else if (!strcmp(CHAR(STRING_ELT(align, 0)), "center"))
    ialign = 0;
  else if (!strcmp(CHAR(STRING_ELT(align, 0)), "left"))
    ialign = -1;
  else
    error("Internal error: invalid align argument in rolling function, should have been caught before. please report to data.table issue tracker."); // # nocov

  if (badaptive && ialign!=1)
    error("using adaptive TRUE and align argument different than 'right' is not implemented");

  SEXP ans = PROTECT(allocVector(VECSXP, nk * nx)); protecti++; // allocate list to keep results
  if (verbose)
    Rprintf("%s: allocating memory for results %dx%d\n", __func__, nx, nk);
  ans_t *dans = (ans_t *)R_alloc(nx*nk, sizeof(ans_t));         // answer columns as array of ans_t struct
  double* dx[nx];                                               // pointers to source columns
  uint_fast64_t inx[nx];                                        // to not recalculate `length(x[[i]])` we store it in extra array
  for (R_len_t i=0; i<nx; i++) {
    inx[i] = xlength(VECTOR_ELT(x, i));                         // for list input each vector can have different length
    for (R_len_t j=0; j<nk; j++) {
      if (badaptive) {                                          // extra input validation
        if (i > 0 && (inx[i]!=inx[i-1]))                        // variable length list input not allowed for adaptive roll
          error("adaptive rolling function can only process 'x' having equal length of elements, like data.table or data.frame; If you want to call rolling function on list having variable length of elements call it for each field separately");
        if (xlength(VECTOR_ELT(kl, j))!=inx[0])                 // check that length of integer vectors in n list match to xrows[0] ([0] and not [i] because there is above check for equal xrows)
          error("length of integer vector(s) provided as list to 'n' argument must be equal to number of observations provided in 'x'");
      }
      SET_VECTOR_ELT(ans, i*nk+j, allocVector(REALSXP, inx[i]));// allocate answer vector for this column-window
      dans[i*nk+j] = ((ans_t) { .dbl_v=REAL(VECTOR_ELT(ans, i*nk+j)), .status=0, .message={"\0","\0","\0","\0"} });
    }
    dx[i] = REAL(VECTOR_ELT(x, i));                             // assign source columns to C pointers
  }

  enum {MEAN, SUM} sfun;
  if (!strcmp(CHAR(STRING_ELT(fun, 0)), "mean"))
    sfun = MEAN;
  else if (!strcmp(CHAR(STRING_ELT(fun, 0)), "sum"))
    sfun = SUM;
  else
    error("Internal error: invalid fun argument in rolling function, should have been caught before. please report to data.table issue tracker."); // # nocov

  if (length(fill) != 1)
    error("fill must be a vector of length 1");

  double dfill;
  if (isInteger(fill)) {
    if (INTEGER(fill)[0]==NA_LOGICAL) {
      dfill = NA_REAL;
    } else {
      dfill = (double)INTEGER(fill)[0];
    }
  } else if (isReal(fill)) {
    dfill = REAL(fill)[0];
  } else if (isLogical(fill) && LOGICAL(fill)[0]==NA_LOGICAL){
    dfill = NA_REAL;
  } else {
    error("fill must be numeric");
  }

  bool bnarm = LOGICAL(narm)[0];

  int ihasna =                                                  // plain C tri-state boolean as integer
    LOGICAL(hasna)[0]==NA_LOGICAL ? 0 :                         // hasna NA, default, no info about NA
    LOGICAL(hasna)[0]==TRUE ? 1 :                               // hasna TRUE, might be some NAs
    -1;                                                         // hasna FALSE, there should be no NAs

  unsigned int ialgo;                                           // decode algo to integer
  if (!strcmp(CHAR(STRING_ELT(algo, 0)), "fast"))
    ialgo = 0;                                                  // fast = 0
  else if (!strcmp(CHAR(STRING_ELT(algo, 0)), "exact"))
    ialgo = 1;                                                  // exact = 1
  else error("Internal error: invalid algo argument in rolling function, should have been caught before. please report to data.table issue tracker."); // # nocov

  int* iik = NULL;
  if (!badaptive) {
    if (!isInteger(ik)) error("Internal error: badaptive=%d but ik is not integer", badaptive);    // # nocov
    iik = INTEGER(ik);                                          // pointer to non-adaptive window width, still can be vector when doing multiple windows
  } else {
    // ik is still R_NilValue from initialization. But that's ok as it's only needed below when !badaptive.
  }

  if (verbose) {
    if (ialgo==0)
      Rprintf("%s: %d column(s) and %d window(s), if product > 1 then entering parallel execution\n", __func__, nx, nk);
    else if (ialgo==1)
      Rprintf("%s: %d column(s) and %d window(s), not entering parallel execution here because algo='exact' will compute results in parallel\n", __func__, nx, nk);
  }
  #pragma omp parallel for if (ialgo==0 && nx*nk>1) schedule(auto) collapse(2) num_threads(getDTthreads())
  for (R_len_t i=0; i<nx; i++) {                                // loop over multiple columns
    for (R_len_t j=0; j<nk; j++) {                              // loop over multiple windows
      switch (sfun) {
      case MEAN :
        if (!badaptive)
          frollmean(ialgo, dx[i], inx[i], &dans[i*nk+j], iik[j], ialign, dfill, bnarm, ihasna, verbose);
        else
          fadaptiverollmean(ialgo, dx[i], inx[i], &dans[i*nk+j], ikl[j], dfill, bnarm, ihasna, verbose);
        break;
      case SUM :
        if (!badaptive)
          frollsum(ialgo, dx[i], inx[i], &dans[i*nk+j], iik[j], ialign, dfill, bnarm, ihasna, verbose);
        else
          fadaptiverollsum(ialgo, dx[i], inx[i], &dans[i*nk+j], ikl[j], dfill, bnarm, ihasna, verbose);
        break;
      default:
        error("Internal error: Unknown sfun value in froll: %d", sfun); // #nocov
      }
    }
  }

  for (R_len_t i=0; i<nx; i++) {                                // raise errors and warnings, as of now messages are not being produced
    for (R_len_t j=0; j<nk; j++) {
      if (verbose && (dans[i*nk+j].message[0][0] != '\0'))
        Rprintf("%s: %d:\n%s", __func__, i*nk+j+1, dans[i*nk+j].message[0]);
      if (dans[i*nk+j].message[1][0] != '\0')
        REprintf("%s: %d:\n%s", __func__, i*nk+j+1, dans[i*nk+j].message[1]); // # nocov because no messages yet // change REprintf according to comments in #3483 when ready
      if (dans[i*nk+j].message[2][0] != '\0')
        warning("%s: %d:\n%s", __func__, i*nk+j+1, dans[i*nk+j].message[2]);
      if (dans[i*nk+j].status == 3) {
        error("%s: %d: %s", __func__, i*nk+j+1, dans[i*nk+j].message[3]); // # nocov because only caused by malloc
      }
    }
  }

  if (verbose)
    Rprintf("%s: processing of %d column(s) and %d window(s) took %.3fs\n", __func__, nx, nk, omp_get_wtime()-tic);

  UNPROTECT(protecti);
  return isVectorAtomic(obj) && length(ans) == 1 ? VECTOR_ELT(ans, 0) : ans;
}

SEXP frollapplyR(SEXP fun, SEXP obj, SEXP k, SEXP fill, SEXP align, SEXP rho) {
  int protecti = 0;
  const bool verbose = GetVerbose();

  if (!isFunction(fun))
    error("internal error: 'fun' must be a function"); // # nocov
  if (!isEnvironment(rho))
    error("internal error: 'rho' should be an environment"); // # nocov

  if (!xlength(obj))
    return(obj);
  double tic = 0;
  if (verbose)
    tic = omp_get_wtime();
  SEXP x = PROTECT(coerceToRealListR(obj)); protecti++;
  R_len_t nx = length(x);

  if (!isInteger(k)) {
    if (isReal(k)) {
      if (isRealReallyInt(k)) {
        SEXP ik = PROTECT(coerceVector(k, INTSXP)); protecti++;
        k = ik;
      } else {
        error("n must be integer");
      }
    } else {
      error("n must be integer");
    }
  }
  R_len_t nk = length(k);
  if (nk == 0)
    error("n must be non 0 length");
  int *ik = INTEGER(k);

  int ialign;
  if (!strcmp(CHAR(STRING_ELT(align, 0)), "right"))
    ialign = 1;
  else if (!strcmp(CHAR(STRING_ELT(align, 0)), "center"))
    ialign = 0;
  else if (!strcmp(CHAR(STRING_ELT(align, 0)), "left"))
    ialign = -1;
  else
    error("Internal error: invalid align argument in rolling function, should have been caught before. please report to data.table issue tracker."); // # nocov

  if (length(fill) != 1)
    error("fill must be a vector of length 1");
  double dfill;
  if (isInteger(fill)) {
    if (INTEGER(fill)[0]==NA_LOGICAL) {
      dfill = NA_REAL;
    } else {
      dfill = (double)INTEGER(fill)[0];
    }
  } else if (isReal(fill)) {
    dfill = REAL(fill)[0];
  } else if (isLogical(fill) && LOGICAL(fill)[0]==NA_LOGICAL){
    dfill = NA_REAL;
  } else {
    error("fill must be numeric");
  }

  SEXP ans = PROTECT(allocVector(VECSXP, nk * nx)); protecti++;
  if (verbose)
    Rprintf("%s: allocating memory for results %dx%d\n", __func__, nx, nk);
  ans_t *dans = (ans_t *)R_alloc(nx*nk, sizeof(ans_t));
  double* dx[nx];
  uint_fast64_t inx[nx];
  for (R_len_t i=0; i<nx; i++) {
    inx[i] = xlength(VECTOR_ELT(x, i));
    for (R_len_t j=0; j<nk; j++) {
      SET_VECTOR_ELT(ans, i*nk+j, allocVector(REALSXP, inx[i]));
      dans[i*nk+j] = ((ans_t) { .dbl_v=REAL(VECTOR_ELT(ans, i*nk+j)), .status=0, .message={"\0","\0","\0","\0"} });
    }
    dx[i] = REAL(VECTOR_ELT(x, i));
  }

  double *dw[nk];
  SEXP pw[nk], pc[nk];
  // in the following loop we handle vectorized k argument
  // for each k we need to allocate own width window object: pw
  // we also need to construct distinct R call pointing to that window
  for (R_len_t j=0; j<nk; j++) {
    pw[j] = PROTECT(allocVector(REALSXP, ik[j])); protecti++;
    dw[j] = REAL(pw[j]);
    pc[j] = PROTECT(LCONS(fun, LCONS(pw[j], LCONS(R_DotsSymbol, R_NilValue)))); protecti++;
  }

  for (R_len_t i=0; i<nx; i++) {
    for (R_len_t j=0; j<nk; j++) {
      frollapply(dx[i], inx[i], dw[j], ik[j], &dans[i*nk+j], ialign, dfill, pc[j], rho, verbose);
    }
  }

  if (verbose)
    Rprintf("%s: processing of %d column(s) and %d window(s) took %.3fs\n", __func__, nx, nk, omp_get_wtime()-tic);

  UNPROTECT(protecti);
  return isVectorAtomic(obj) && length(ans) == 1 ? VECTOR_ELT(ans, 0) : ans;
}
