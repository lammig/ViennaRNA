/* Last changed Time-stamp: <2008-12-03 17:44:38 ivo> */
/*
                  minimum free energy
                  RNA secondary structure prediction

                  c Ivo Hofacker, Chrisoph Flamm
                  original implementation by
                  Walter Fontana

                  Vienna RNA package
*/

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>

#include "ViennaRNA/utils.h"
#include "ViennaRNA/energy_par.h"
#include "ViennaRNA/fold_vars.h"
#include "ViennaRNA/params.h"
#include "ViennaRNA/subopt.h"
#include "ViennaRNA/fold.h"
#include "ViennaRNA/loop_energies.h"
#include "ViennaRNA/gquad.h"
#include "ViennaRNA/cofold.h"

#ifdef  VRNA_BACKWARD_COMPAT

#ifdef _OPENMP
#include <omp.h>
#endif

#endif

#define MAXSECTORS        500     /* dimension for a backtrack array */
#define TURN2             3       /* used by zukersubopt */
#define ON_SAME_STRAND(I,J,C)  (((I)>=(C))||((J)<(C)))

/*
#################################
# GLOBAL VARIABLES              #
#################################
*/


/*
#################################
# PRIVATE VARIABLES             #
#################################
*/

#ifdef  VRNA_BACKWARD_COMPAT

/* some backward compatibility stuff */
PRIVATE int                 backward_compat           = 0;
PRIVATE vrna_fold_compound  *backward_compat_compound = NULL;

PRIVATE float   mfe1, mfe2;       /* minimum free energies of the monomers */

#ifdef _OPENMP

#pragma omp threadprivate(mfe1, mfe2, backward_compat_compound, backward_compat)

#endif

#endif

/*
#################################
# PRIVATE FUNCTION DECLARATIONS #
#################################
*/

PRIVATE void  backtrack(sect bt_stack[], bondT *bp_list, vrna_fold_compound *vc);
PRIVATE int   fill_arrays(vrna_fold_compound *vc, int zuker);
PRIVATE void  free_end(int *array, int i, int start, vrna_fold_compound *vc);

#ifdef  VRNA_BACKWARD_COMPAT

/* wrappers for old API compatibility */
PRIVATE void      wrap_array_export(int **f5_p,int **c_p,int **fML_p,int **fM1_p,int **fc_p,int **indx_p,char **ptype_p);
PRIVATE float     wrap_cofold(const char *string,char *structure,vrna_param_t *parameters,int is_constrained);
PRIVATE SOLUTION *wrap_zukersubopt( const char *string,vrna_param_t *parameters);

#endif

/*
#################################
# BEGIN OF FUNCTION DEFINITIONS #
#################################
*/

PUBLIC float
vrna_cofold(vrna_fold_compound  *vc,
            char                *structure){

  int     length, energy;
  sect    bt_stack[MAXSECTORS]; /* stack of partial structures for backtracking */
  bondT   *bp;

  length = (int) vc->length;

  vc->sequence_encoding[0] = vc->sequence_encoding2[0]; /* store length at pos. 0 in S1 too */

  if(vc->sc)
    if(vc->sc->pre)
      vc->sc->pre(vc, VRNA_SC_GEN_MFE);

  energy = fill_arrays(vc, 0);

  if(structure && vc->params->model_details.backtrack){
    bp = (bondT *)vrna_alloc(sizeof(bondT) * (4*(1+length/2))); /* add a guess of how many G's may be involved in a G quadruplex */

    backtrack(bt_stack, bp, vc);

    vrna_parenthesis_structure(structure, bp, length);

#ifdef  VRNA_BACKWARD_COMPAT

    /*
    *  Backward compatibility:
    *  This block may be removed if deprecated functions
    *  relying on the global variable "base_pair" vanish from within the package!
    */
    {
      if(base_pair) free(base_pair);
      base_pair = bp;
    }

#endif

  }

  if(vc->sc)
    if(vc->sc->post)
      vc->sc->post(vc, VRNA_SC_GEN_MFE);

  if (vc->params->model_details.backtrack_type=='C')
    return (float) vc->matrices->c[vc->jindx[length]+1]/100.;
  else if (vc->params->model_details.backtrack_type=='M')
    return (float) vc->matrices->fML[vc->jindx[length]+1]/100.;
  else
    return (float) energy/100.;
}

PRIVATE int
fill_arrays(vrna_fold_compound  *vc,
            int                 zuker){

  /* fill "c", "fML" and "f5" arrays and return  optimal energy */

  int   i, j, length, energy;
  int   cp, uniq_ML;
  int   no_close, type, maxj, *indx;
  int   *my_f5, *my_c, *my_fML, *my_fM1, *my_fc;
  int   *cc, *cc1;  /* auxilary arrays for canonical structures     */
  int   *Fmi;       /* holds row i of fML (avoids jumps in memory)  */
  int   *DMLi;      /* DMLi[j] holds  MIN(fML[i,k]+fML[k+1,j])      */
  int   *DMLi1;     /*                MIN(fML[i+1,k]+fML[k+1,j])    */
  int   *DMLi2;     /*                MIN(fML[i+2,k]+fML[k+1,j])    */

  int   dangle_model, noGUclosure, noLP, hc_decompose, turn;
  char              *ptype, *hard_constraints;
  vrna_param_t      *P;
  vrna_mx_mfe_t     *matrices;
  vrna_hc_t         *hc;

  length            = (int)vc->length;
  ptype             = vc->ptype;
  indx              = vc->jindx;
  P                 = vc->params;
  dangle_model      = P->model_details.dangles;
  noGUclosure       = P->model_details.noGUclosure;
  noLP              = P->model_details.noLP;
  uniq_ML           = P->model_details.uniq_ML;
  hc                = vc->hc;
  hard_constraints  = hc->matrix;
  matrices          = vc->matrices;
  my_f5             = matrices->f5;
  my_c              = matrices->c;
  my_fML            = matrices->fML;
  my_fM1            = matrices->fM1;
  my_fc             = matrices->fc;
  cp                = vc->cutpoint;
  turn              = P->model_details.min_loop_size;

  /* allocate memory for all helper arrays */
  cc    = (int *) vrna_alloc(sizeof(int)*(length + 2));
  cc1   = (int *) vrna_alloc(sizeof(int)*(length + 2));
  Fmi   = (int *) vrna_alloc(sizeof(int)*(length + 1));
  DMLi  = (int *) vrna_alloc(sizeof(int)*(length + 1));
  DMLi1 = (int *) vrna_alloc(sizeof(int)*(length + 1));
  DMLi2 = (int *) vrna_alloc(sizeof(int)*(length + 1));


  /* hard code min_loop_size to 0, since we can not be sure yet that this is already the case */
  turn = 0;

  for (j=1; j<=length; j++) {
    Fmi[j]=DMLi[j]=DMLi1[j]=DMLi2[j]=INF;
    my_fc[j]=0;
  }

  for (j = 1; j<=length; j++)
    for (i=1; i<=j; i++) {
      my_c[indx[j]+i] = my_fML[indx[j]+i] = INF;
      if (uniq_ML) my_fM1[indx[j]+i] = INF;
    }

  for (i = length-turn-1; i >= 1; i--) { /* i,j in [1..length] */

    maxj=(zuker)? (MIN2(i+cp-1,length)):length;
    for (j = i+turn+1; j <= maxj; j++) {
      int ij;
      ij            = indx[j]+i;
      type          = (unsigned char)ptype[ij];
      hc_decompose  = hard_constraints[ij];
      energy        = INF;

      no_close = (((type==3)||(type==4))&&noGUclosure);

      if (hc_decompose) {   /* we have a pair */
        int new_c = INF;

        if(!no_close){
          /* check for hairpin loop */
          energy  = vrna_E_hp_loop(vc, i, j);
          new_c   = MIN2(new_c, energy);

          /* check for multibranch loops */
          energy  = E_mb_loop_fast(i, j, vc, DMLi1, DMLi2);
          new_c   = MIN2(new_c, energy);
        }

        if (dangle_model==3) { /* coaxial stacking */
          energy  = E_mb_loop_stack(i, j, vc);
          new_c   = MIN2(new_c, energy);
        }

        /* check for interior loops */
        energy = vrna_E_int_loop(vc, i, j);
        new_c = MIN2(new_c, energy);

        /* remember stack energy for --noLP option */
        if(noLP){
          int stackEnergy = vrna_E_stack(vc, i, j);
          new_c           = MIN2(new_c, cc1[j-1]+stackEnergy);
          cc[j]           = new_c;

          if (ON_SAME_STRAND(i,i+1,cp) && ON_SAME_STRAND(j-1,j,cp))
            my_c[ij] = cc1[j-1]+stackEnergy;
          else /* currently we don't allow stacking over the cut point */
            my_c[ij] = FORBIDDEN;

        } else {
          my_c[ij] = new_c;
        }
      } /* end >> if (pair) << */

      else my_c[ij] = INF;

      /* done with c[i,j], now compute fML[i,j] */
      /* free ends ? -----------------------------------------*/

      my_fML[ij] = E_ml_stems_fast(i, j, vc, Fmi, DMLi);

      if(uniq_ML){  /* compute fM1 for unique decomposition */
        my_fM1[ij] = E_ml_rightmost_stem(i, j, vc);
      }

    }

    if (i==cp)
      for (j=i; j<=maxj; j++)
        free_end(my_fc, j, cp, vc);
    if (i<cp)
      free_end(my_fc,i,cp-1, vc);


    {
      int *FF; /* rotate the auxilliary arrays */
      FF = DMLi2; DMLi2 = DMLi1; DMLi1 = DMLi; DMLi = FF;
      FF = cc1; cc1=cc; cc=FF;
      for (j=1; j<=maxj; j++) {cc[j]=Fmi[j]=DMLi[j]=INF; }
    }
  }

  /* calculate energies of 5' and 3' fragments */

  for (i=1; i<=length; i++)
    free_end(my_f5, i, 1, vc);

  if (cp>0) {
    mfe1  = my_f5[cp-1];
    mfe2  = my_fc[length];
    /* add DuplexInit, check whether duplex*/
    for (i=cp; i<=length; i++) {
      my_f5[i] = MIN2(my_f5[i]+P->DuplexInit, my_fc[i]+my_fc[1]);
    }
  }

  energy = my_f5[length];
  if (cp<1) mfe1=mfe2=energy;
  return energy;
}

PRIVATE void
backtrack_co( sect bt_stack[],
              bondT *bp_list,
              int s,
              int b, /* b=0: start new structure, b \ne 0: add to existing structure */
              vrna_fold_compound *vc) {

  /*------------------------------------------------------------------
    trace back through the "c", "fc", "f5" and "fML" arrays to get the
    base pairing list. No search for equivalent structures is done.
    This is fast, since only few structure elements are recalculated.
    ------------------------------------------------------------------*/

  int   i, j, ij, k, length, energy, en, new, ml0, ml5, ml3, ml53, no_close, type, type_2, tt;
  char  *string         = vc->sequence;
  vrna_param_t  *P      = vc->params;
  int     *indx         = vc->jindx;
  char    *ptype        = vc->ptype;

  short *S1             = vc->sequence_encoding;
  short *S              = vc->sequence_encoding2;
  int   dangle_model    = P->model_details.dangles;
  int   noLP            = P->model_details.noLP;
  int   noGUclosure     = P->model_details.noGUclosure;
  int   with_gquad      = P->model_details.gquad;
  int   turn            = P->model_details.min_loop_size;
  int   *rtype          = &(P->model_details.rtype[0]);
  char  backtrack_type  = P->model_details.backtrack_type;
  int   cp              = vc->cutpoint;
  vrna_hc_t  *hc        = vc->hc;
  vrna_sc_t  *sc        = vc->sc;
  char      *hard_constraints  = hc->matrix;

  /* the folding matrices */
  int   *my_f5, *my_c, *my_fML, *my_fc, *my_ggg;

  length  = vc->length;
  my_f5   = vc->matrices->f5;
  my_c    = vc->matrices->c;
  my_fML  = vc->matrices->fML;
  my_fc   = vc->matrices->fc;
  my_ggg  = vc->matrices->ggg;

  /* int   b=0;*/

  /* hard code min_loop_size to 0, since we can not be sure yet that this is already the case */
  turn = 0;

  length = strlen(string);
  if (s==0) {
    bt_stack[++s].i = 1;
    bt_stack[s].j   = length;
    bt_stack[s].ml  = (backtrack_type=='M') ? 1 : ((backtrack_type=='C')?2:0);
  }
  while (s>0) {
    int ml, fij, fi, cij, traced, i1, j1, mm, p, q, jj=0, gq=0;
    int canonical = 1;     /* (i,j) closes a canonical structure */

    /* pop one element from stack */
    i  = bt_stack[s].i;
    j  = bt_stack[s].j;
    ml = bt_stack[s--].ml;

    switch(ml){
      /* backtrack in f5 */
      case 0: {
                int p, q;
                if(vrna_BT_ext_loop_f5(vc, &j, &p, &q, bp_list, &b)){
                  if(j > 0){
                    bt_stack[++s].i = 1;
                    bt_stack[s].j   = j;
                    bt_stack[s].ml  = 0;
                  }
                  if(p > 0){
                    i = p;
                    j = q;
                    goto repeat1;
                  }

                  continue;
                } else {
                  fprintf(stderr, "%s\n", string);
                  vrna_message_error("backtrack failed in f5");
                }
              }
              break;

      /* true multi-loop backtrack in fML */
      case 1: {
                int p, q, comp1, comp2;
                if(vrna_BT_mb_loop_split(vc, &i, &j, &p, &q, &comp1, &comp2, bp_list, &b)){
                  if(i > 0){
                    bt_stack[++s].i = i;
                    bt_stack[s].j   = j;
                    bt_stack[s].ml  = comp1;
                  }
                  if(p > 0){
                    bt_stack[++s].i = p;
                    bt_stack[s].j   = q;
                    bt_stack[s].ml  = comp2;
                  }

                  continue;
                } else {
                  fprintf(stderr, "%s\n", string);
                  vrna_message_error("backtrack failed in fML");
                }
              }
              break;

      case 2: bp_list[++b].i = i;
              bp_list[b].j   = j;
              goto repeat1;

      /* backtrack fake-multi loop parts */
      case 3: case 4:
              {
                int lower, k, p, q;
                p = i;
                q = j;
                lower = (i < cp) ? 1 : 0;

                if(vrna_BT_mb_loop_fake(vc, &k, &i, &j, bp_list, &b)){
                  if(k > 0){
                    bt_stack[++s].i = (lower) ? k : p;
                    bt_stack[s].j   = (lower) ? q : k;
                    bt_stack[s].ml  = ml;
                  }
                  if(i > 0){
                    goto repeat1;
                  }

                  continue;
                } else {
                  fprintf(stderr, "%s\n", string);
                  vrna_message_error("backtrack failed in fc");
                }
              }
              break;
    } /* end of switch(ml) */

  repeat1:

    /*----- begin of "repeat:" -----*/
    ij = indx[j]+i;

    if (canonical)
      cij = my_c[ij];

    type = ptype[ij];

    if (noLP)
      if(vrna_BT_stack(vc, &i, &j, &cij, bp_list, &b)){
        canonical = 0;
        goto repeat1;
      }

    canonical = 1;

    no_close = (((type==3)||(type==4))&&noGUclosure);
    if (no_close) {
      if (cij == FORBIDDEN) continue;
    } else {
      if(vrna_BT_hp_loop(vc, i, j, cij, bp_list, &b))
        continue;
    }

    if(vrna_BT_int_loop(vc, &i, &j, cij, bp_list, &b)){
      if(i < 0)
        continue;
      else
        goto repeat1;
    }

    /* (i.j) must close a fake or true multi-loop */
    int comp1, comp2;

    if(vrna_BT_mb_loop(vc, &i, &j, &k, cij, &comp1, &comp2)){
      bt_stack[++s].i = i;
      bt_stack[s].j   = k;
      bt_stack[s].ml  = comp1;
      bt_stack[++s].i = k + 1;
      bt_stack[s].j   = j;
      bt_stack[s].ml  = comp2;
    } else {
      vrna_message_error("backtracking failed in repeat");
    }

    /* end of repeat: --------------------------------------------------*/

  } /* end >> while (s>0) << */

  bp_list[0].i = b;    /* save the total number of base pairs */
}

PRIVATE void
free_end( int *array,
          int i,
          int start,
          vrna_fold_compound *vc){

  int inc, type, energy, en, length, j, left, right, cp, dangle_model, with_gquad, *indx, *c, *ggg, turn;
  vrna_param_t  *P;
  short         *S1;
  char          *ptype, *hard_constraints;
  vrna_mx_mfe_t *matrices;
  vrna_hc_t     *hc;
  vrna_sc_t     *sc;

  cp            = vc->cutpoint;
  P             = vc->params;
  dangle_model  = P->model_details.dangles;
  with_gquad    = P->model_details.gquad;
  turn          = P->model_details.min_loop_size;
  inc           = (i>start)? 1:-1;
  length        = (int)vc->length;
  S1            = vc->sequence_encoding;
  ptype         = vc->ptype;
  indx          = vc->jindx;
  matrices      = vc->matrices;
  c             = matrices->c;
  ggg           = matrices->ggg;
  hc            = vc->hc;
  sc            = vc->sc;
  hard_constraints  = hc->matrix;

  if(hc->up_ext[i]){
    if (i==start) array[i]=0;
    else array[i] = array[i-inc];
    if(sc){
      if(sc->free_energies)
        array[i] += sc->free_energies[i][1];
    }
  } else
    array[i] = INF;

  if (inc>0) {
    left = start; right=i;
  } else {
    left = i; right = start;
  }

  /* hard code min_loop_size to 0, since we can not be sure yet that this is already the case */
  turn = 0;

  for (j=start; inc*(i-j)>turn; j+=inc) {
    int ii, jj;
    short si, sj;
    if (i>j) { ii = j; jj = i;} /* inc>0 */
    else     { ii = i; jj = j;} /* inc<0 */
    type = ptype[indx[jj]+ii];
    if(hard_constraints[indx[jj]+ii] & VRNA_CONSTRAINT_CONTEXT_EXT_LOOP){
      si = (ii>1)       && ON_SAME_STRAND(ii-1,ii,cp) ? S1[ii-1] : -1;
      sj = (jj<length)  && ON_SAME_STRAND(jj,jj+1,cp) ? S1[jj+1] : -1;
      energy = c[indx[jj]+ii];
      if(energy != INF){
        switch(dangle_model){
          case 0:   if(array[j-inc] != INF){
                      en = array[j-inc] + energy + E_ExtLoop(type, -1, -1, P);
                      array[i] = MIN2(array[i], en);
                    }
                    break;
          case 2:   if(array[j-inc] != INF){
                      en = array[j-inc] + energy + E_ExtLoop(type, si, sj, P);
                      array[i] = MIN2(array[i], en);
                    }
                    break;
          default:  if(array[j-inc] != INF){
                      en = array[j-inc] + energy + E_ExtLoop(type, -1, -1, P);
                      array[i] = MIN2(array[i], en);
                    }
                    if(inc > 0){
                      if(j > left){
                        if(hc->up_ext[ii-1]){
                          if(array[j-2] != INF){
                            en = array[j-2] + energy + E_ExtLoop(type, si, -1, P);
                            if(sc)
                              if(sc->free_energies)
                                en += sc->free_energies[ii-1][1];

                            array[i] = MIN2(array[i], en);
                          }
                        }
                      }
                    } else if(j < right){
                      if(hc->up_ext[jj+1]){
                        if(array[j+2] != INF){
                          en = array[j+2] + energy + E_ExtLoop(type, -1, sj, P);
                          if(sc)
                            if(sc->free_energies)
                              en += sc->free_energies[jj+1][1];

                          array[i] = MIN2(array[i], en);
                        }
                      }
                    }
                    break;
        }
      }
    }

    if(with_gquad){
      if(ON_SAME_STRAND(ii, jj,cp))
        if(array[j-inc] != INF)
          array[i] = MIN2(array[i], array[j-inc] + ggg[indx[jj]+ii]);
    }

    if (dangle_model%2==1) {
      /* interval ends in a dangle (i.e. i-inc is paired) */
      if (i>j) { ii = j; jj = i-1;} /* inc>0 */
      else     { ii = i+1; jj = j;} /* inc<0 */

      if (!(hard_constraints[indx[jj]+ii] & VRNA_CONSTRAINT_CONTEXT_EXT_LOOP))
        continue;

      type = ptype[indx[jj]+ii];
      si = (ii > left)  && ON_SAME_STRAND(ii-1,ii,cp) ? S1[ii-1] : -1;
      sj = (jj < right) && ON_SAME_STRAND(jj,jj+1,cp) ? S1[jj+1] : -1;
      energy = c[indx[jj]+ii];
      if(energy != INF){
        if(inc>0){
          if(hc->up_ext[jj-1]){
            if(array[j-inc] != INF){
              en = array[j - inc] + energy + E_ExtLoop(type, -1, sj, P);
              if(sc)
                if(sc->free_energies)
                  en += sc->free_energies[jj+1][1];

              array[i] = MIN2(array[i], en);
            }
          }
        } else {
          if(hc->up_ext[ii-1]){
            if(array[j - inc] != INF){
              en = array[j - inc] + energy + E_ExtLoop(type, si, -1, P);
              if(sc)
                if(sc->free_energies)
                  en += sc->free_energies[ii-1][1];

              array[i] = MIN2(array[i], en);
            }
          }
        }
        if(j!= start){ /* dangle_model on both sides */
          if(hc->up_ext[jj-1] && hc->up_ext[ii-1]){
            if(array[j-2*inc] != INF){
              en = array[j-2*inc] + energy + E_ExtLoop(type, si, sj, P);
              if(sc)
                if(sc->free_energies)
                  en += sc->free_energies[ii-1][1] + sc->free_energies[jj+1][1];

              array[i] = MIN2(array[i], en);
            }
          }
        }
      }
    }
  }
}

PRIVATE void
backtrack(sect bt_stack[],
          bondT *bp_list,
          vrna_fold_compound *vc){

  /*routine to call backtrack_co from 1 to n, backtrack type??*/
  backtrack_co(bt_stack, bp_list, 0,0, vc);
}

#if 0
PRIVATE int comp_pair(const void *A, const void *B) {
  bondT *x,*y;
  int ex, ey;
  x = (bondT *) A;
  y = (bondT *) B;
  ex = c[indx[x->j]+x->i]+c[indx[x->i+length]+x->j];
  ey = c[indx[y->j]+y->i]+c[indx[y->i+length]+y->j];
  if (ex>ey) return 1;
  if (ex<ey) return -1;
  return (indx[x->j]+x->i - indx[y->j]+y->i);
}
#endif

PUBLIC SOLUTION *
vrna_zukersubopt(vrna_fold_compound *vc){

/* Compute zuker suboptimal. Here, we're abusing the cofold() code
   "double" sequence, compute dimerarray entries, track back every base pair.
   This is slightly wasteful compared to the normal solution */

  char          *structure, *mfestructure, **todo, *ptype;
  int           i, j, counter, num_pairs, psize, p, *indx, *c;
  unsigned int  length, doublelength;
  float         energy;
  SOLUTION      *zukresults;
  bondT         *pairlist, *bp_list;
  sect          bt_stack[MAXSECTORS]; /* stack of partial structures for backtracking */
  vrna_mx_mfe_t *matrices;

  doublelength    = vc->length;
  length          = doublelength/2;
  indx            = vc->jindx;
  ptype           = vc->ptype;
  matrices        = vc->matrices;
  c               = matrices->c;
  num_pairs       = counter = 0;
  mfestructure    = (char *) vrna_alloc((unsigned) doublelength+1);
  structure       = (char *) vrna_alloc((unsigned) doublelength+1);
  zukresults      = (SOLUTION *)vrna_alloc(((length*(length-1))/2)*sizeof(SOLUTION));
  mfestructure[0] = '\0';

  /* store length at pos. 0 */
  vc->sequence_encoding[0] = vc->sequence_encoding2[0];

  /* get mfe and do forward recursion */
  (void)fill_arrays(vc, 1);

  psize     = length;
  pairlist  = (bondT *) vrna_alloc(sizeof(bondT)*(psize+1));
  bp_list   = (bondT *) vrna_alloc(sizeof(bondT) * (1 + length/2));
  todo      = (char **) vrna_alloc(sizeof(char *)*(length+1));
  for (i=1; i<length; i++) {
    todo[i] = (char *) vrna_alloc(sizeof(char)*(length+1));
  }

  /* Make a list of all base pairs */
  for (i=1; i<length; i++) {
    for (j=i+TURN2+1/*??*/; j<=length; j++) {
      if (ptype[indx[j]+i]==0) continue;
      if (num_pairs>=psize) {
        psize = 1.2*psize + 32;
        pairlist = vrna_realloc(pairlist, sizeof(bondT)*(psize+1));
      }
      pairlist[num_pairs].i   = i;
      pairlist[num_pairs++].j = j;
      todo[i][j]=1;
    }
  }

#if 0
  qsort(pairlist, num_pairs, sizeof(bondT), comp_pair);
#endif

  for (p=0; p<num_pairs; p++) {
    i=pairlist[p].i;
    j=pairlist[p].j;
    if (todo[i][j]) {
      int k;
      bt_stack[1].i   = i;
      bt_stack[1].j   = j;
      bt_stack[1].ml  = 2;
      backtrack_co(bt_stack, bp_list, 1,0, vc);
      bt_stack[1].i   = j;
      bt_stack[1].j   = i + length;
      bt_stack[1].ml  = 2;
      backtrack_co(bt_stack, bp_list, 1,bp_list[0].i, vc);
      energy = c[indx[j]+i]+c[indx[i+length]+j];
      vrna_parenthesis_zuker(structure, bp_list, length);
      zukresults[counter].energy      = energy;
      zukresults[counter++].structure = strdup(structure);
      for (k = 1; k <= bp_list[0].i; k++) { /* mark all pairs in structure as done */
        int x,y;
        x=bp_list[k].i;
        y=bp_list[k].j;
        if (x>length) x-=length;
        if (y>length) y-=length;
        if (x>y) {
          int temp;
          temp=x; x=y; y=temp;
        }
        todo[x][y] = 0;
      }
    }
  }

#ifdef  VRNA_BACKWARD_COMPAT

  /*
  *  Backward compatibility:
  *  This block may be removed if deprecated functions
  *  relying on the global variable "base_pair" vanish from within the package!
  */
  {
    if(base_pair) free(base_pair);
    base_pair = bp_list;
  }

#endif

  /* clean up */
  free(pairlist);
  for (i=1; i<length; i++)
    free(todo[i]);
  free(todo);
  free(structure);
  free(mfestructure);

  return zukresults;
}

PUBLIC char *
vrna_cut_point_insert(const char *string,
                      int cp){

  char *ctmp;
  int len;

  if(cp > 0){
    len = strlen(string);
    ctmp = (char *)vrna_alloc((len+2) * sizeof(char));
    /* first sequence */
    (void) strncpy(ctmp, string, cp-1);
    /* spacer */
    ctmp[cp-1] = '&';
    /* second sequence */
    (void) strcat(ctmp, string+cp-1);
  } else {
    ctmp = strdup(string);
  }
  return ctmp;
}

PUBLIC char *
vrna_cut_point_remove(const char *string,
                      int *cp){

  char *pos, *copy = NULL;

  *cp = -1;

  if(string){
    copy = (char *) vrna_alloc(strlen(string)+1);
    (void) sscanf(string, "%s", copy);
    pos = strchr(copy, '&');
    if (pos) {
      *cp = (int)(pos - copy) + 1;
      if (*cp >= strlen(copy)) *cp = -1;
      if (strchr(pos+1, '&')) vrna_message_error("more than one cut-point in input");
      for (;*pos;pos++) *pos = *(pos+1); /* splice out the & */
    }
  }

  return copy;
}

/*###########################################*/
/*# deprecated functions below              #*/
/*###########################################*/

#ifdef  VRNA_BACKWARD_COMPAT

PRIVATE void
wrap_array_export(int **f5_p,
                  int **c_p,
                  int **fML_p,
                  int **fM1_p,
                  int **fc_p,
                  int **indx_p,
                  char **ptype_p){

  /* make the DP arrays available to routines such as subopt() */
  if(backward_compat_compound){
    *f5_p     = backward_compat_compound->matrices->f5;
    *c_p      = backward_compat_compound->matrices->c;
    *fML_p    = backward_compat_compound->matrices->fML;
    *fM1_p    = backward_compat_compound->matrices->fM1;
    *fc_p     = backward_compat_compound->matrices->fc;
    *indx_p   = backward_compat_compound->jindx;
    *ptype_p  = backward_compat_compound->ptype;
  }
}

/*--------------------------------------------------------------------------*/

PRIVATE float
wrap_cofold(const char *string,
            char *structure,
            vrna_param_t *parameters,
            int is_constrained){

  unsigned int        length;
  char                *seq;
  vrna_fold_compound  *vc;
  vrna_param_t        *P;
  float               mfe;

  vc      = NULL;
  length  = strlen(string);

#ifdef _OPENMP
/* Explicitly turn off dynamic threads */
  omp_set_dynamic(0);
#endif

  /* we need the parameter structure for hard constraints */
  if(parameters)
    P = get_parameter_copy(parameters);
  else{
    vrna_md_t md;
    set_model_details(&md);
    P = get_scaled_parameters(temperature, md);
  }
  P->model_details.min_loop_size = 0;  /* set min loop length to 0 */

  /* dirty hack to reinsert the '&' according to the global variable 'cut_point' */
  seq = vrna_cut_point_insert(string, cut_point);

  /* get compound structure */
  vc = vrna_get_fold_compound(seq, &(P->model_details), VRNA_OPTION_MFE | VRNA_OPTION_HYBRID);

  if(parameters){ /* replace params if necessary */
    free(vc->params);
    vc->params = P;
  } else {
    free(P);
  }

  /* handle hard constraints in pseudo dot-bracket format if passed via simple interface */
  if(is_constrained && structure){
    unsigned int constraint_options = 0;
    constraint_options |= VRNA_CONSTRAINT_DB
                          | VRNA_CONSTRAINT_DB_PIPE
                          | VRNA_CONSTRAINT_DB_DOT
                          | VRNA_CONSTRAINT_DB_X
                          | VRNA_CONSTRAINT_DB_ANG_BRACK
                          | VRNA_CONSTRAINT_DB_RND_BRACK
                          | VRNA_CONSTRAINT_DB_INTRAMOL
                          | VRNA_CONSTRAINT_DB_INTERMOL;

    vrna_add_constraints(vc, (const char *)structure, constraint_options);
  }

  if(backward_compat_compound)
    vrna_free_fold_compound(backward_compat_compound);

  backward_compat_compound  = vc;
  backward_compat           = 1;

  /* cleanup */
  free(seq);

  return vrna_cofold(vc, structure);
}

PRIVATE SOLUTION *
wrap_zukersubopt( const char *string,
                  vrna_param_t *parameters){

  unsigned int        length;
  char                *doubleseq;
  vrna_fold_compound  *vc;
  vrna_param_t        *P;

  vc      = NULL;
  length  = (int)strlen(string);

#ifdef _OPENMP
/* Explicitly turn off dynamic threads */
  omp_set_dynamic(0);
#endif

  /* we need the parameter structure for hard constraints */
  if(parameters)
    P = get_parameter_copy(parameters);
  else{
    vrna_md_t md;
    set_model_details(&md);
    P = get_scaled_parameters(temperature, md);
  }
  P->model_details.min_loop_size = 0;  /* set min loop length to 0 */

  doubleseq = (char *)vrna_alloc((2*length+2)*sizeof(char));
  strcpy(doubleseq,string);
  doubleseq[length] = '&';
  strcat(doubleseq, string);

  /* get compound structure */
  vc = vrna_get_fold_compound(doubleseq, &(P->model_details), VRNA_OPTION_MFE | VRNA_OPTION_HYBRID);

  if(parameters){ /* replace params if necessary */
    free(vc->params);
    vc->params = P;
  } else {
    free(P);
  }

  if(backward_compat_compound)
    vrna_free_fold_compound(backward_compat_compound);

  backward_compat_compound  = vc;
  backward_compat           = 1;

  /* cleanup */
  free(doubleseq);

  return vrna_zukersubopt(vc);
}

PUBLIC void
initialize_cofold(int length){ /* DO NOTHING */ }

PUBLIC void
free_co_arrays(void){

  if(backward_compat_compound && backward_compat){
    vrna_free_fold_compound(backward_compat_compound);
    backward_compat_compound  = NULL;
    backward_compat           = 0;
  }
}


/*--------------------------------------------------------------------------*/

PUBLIC void
export_cofold_arrays_gq(int **f5_p,
                        int **c_p,
                        int **fML_p,
                        int **fM1_p,
                        int **fc_p,
                        int **ggg_p,
                        int **indx_p,
                        char **ptype_p){

  /* make the DP arrays available to routines such as subopt() */
  wrap_array_export(f5_p, c_p, fML_p, fM1_p, fc_p, indx_p, ptype_p);
  if(backward_compat_compound){
    *ggg_p = backward_compat_compound->matrices->ggg;
  }
}

PUBLIC void
export_cofold_arrays( int **f5_p,
                      int **c_p,
                      int **fML_p,
                      int **fM1_p,
                      int **fc_p,
                      int **indx_p,
                      char **ptype_p){

  wrap_array_export(f5_p, c_p, fML_p, fM1_p, fc_p, indx_p, ptype_p);
}

PUBLIC float
cofold( const char *string,
        char *structure){

  return wrap_cofold(string, structure, NULL, fold_constrained);
}

PUBLIC float
cofold_par( const char *string,
            char *structure,
            vrna_param_t *parameters,
            int is_constrained){

  return wrap_cofold(string, structure, parameters, is_constrained);
}

PUBLIC SOLUTION *
zukersubopt(const char *string) {

  return wrap_zukersubopt(string, NULL);
}

PUBLIC SOLUTION *
zukersubopt_par(const char *string,
                vrna_param_t *parameters){

  return wrap_zukersubopt(string, parameters);
}

PUBLIC void
update_cofold_params(void){

  vrna_fold_compound *v;
  
  if(backward_compat_compound && backward_compat){
    vrna_md_t md;
    v = backward_compat_compound;

    if(v->params)
      free(v->params);

    set_model_details(&md);
    v->params = vrna_params_get(&md);
  }
}

PUBLIC void
update_cofold_params_par(vrna_param_t *parameters){

  vrna_fold_compound *v;
  
  if(backward_compat_compound && backward_compat){
    v = backward_compat_compound;

    if(v->params)
      free(v->params);

    if(parameters){
      v->params = get_parameter_copy(parameters);
    } else {
      vrna_md_t md;
      set_model_details(&md);
      v->params = vrna_params_get(&md);
    }
  }
}

PUBLIC void get_monomere_mfes(float *e1, float *e2) {
  /*exports monomere free energies*/
  *e1 = mfe1;
  *e2 = mfe2;
}

#endif
