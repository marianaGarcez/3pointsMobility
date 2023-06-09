
/*****************************************************************************
 *
 * This MobilityDB code is provided under The PostgreSQL License.
 * Copyright (c) 2016-2023, Université libre de Bruxelles and MobilityDB
 * contributors
 *
 * MobilityDB includes portions of PostGIS version 3 source code released
 * under the GNU General Public License (GPLv2 or later).
 * Copyright (c) 2001-2023, PostGIS contributors
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice and
 * this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL UNIVERSITE LIBRE DE BRUXELLES BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
 * EVEN IF UNIVERSITE LIBRE DE BRUXELLES HAS BEEN ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * UNIVERSITE LIBRE DE BRUXELLES SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON
 * AN "AS IS" BASIS, AND UNIVERSITE LIBRE DE BRUXELLES HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS. 
 *
 *****************************************************************************/

/**
 * @brief Mathematical operators (+, -, *, /) and functions (round, degrees,...)
 * for temporal numbers.
 */

#include "general/tnumber_mathfuncs.h"

/* C */
#include <assert.h>
#include <math.h>
/* PostgreSQL */
#include <postgres.h>
#include <utils/float.h>
/* MEOS */
#include <meos.h>
#include <meos_internal.h>
#include "general/lifting.h"
#include "general/pg_types.h"
#include "general/temporaltypes.h"
#include "general/type_util.h"
#include "general/tsequence.h"
#include "point/tpoint.h"
#include "point/tpoint_boxops.h"
#include "point/tpoint_distance.h"
#include "point/tpoint_spatialfuncs.h"
#include "point/tpoint_spatialrels.h"
#include "point/geography_funcs.h"

/*****************************************************************************
 * Miscellaneous functions on datums
 *****************************************************************************/

/**
 * @ingroup libmeos_temporal_math
 * @brief Convert a number from radians to degrees
 * @sqlfunc degrees()
 */
double
float_degrees(double value, bool normalize)
{
  double result = float8_div(value, RADIANS_PER_DEGREE);
  if (normalize)
  {
    /* The value would be in the range (-360, 360) */
    result = fmod(result, 360.0);
    if (result < 0)
      result += 360.0; /* The value would be in the range [0, 360) */
  }
  return result;
}

/**
 * @brief Convert a number from radians to degrees
 */
static Datum
datum_degrees(Datum value, Datum normalize)
{
  return Float8GetDatum(float_degrees(DatumGetFloat8(value),
    DatumGetBool(normalize)));
}

/**
 * @brief Convert a number from degrees to radians
 */
static Datum
datum_radians(Datum value)
{
  return Float8GetDatum(float8_mul(DatumGetFloat8(value), RADIANS_PER_DEGREE));
}

/**
 * @brief Find the single timestamptz at which the operation of two temporal
 * number segments is at a local minimum/maximum. The function supposes that
 * the instants are synchronized, that is, start1->t = start2->t and
 * end1->t = end2->t. The function only return an intersection at the middle,
 * that is, it returns false if the timestamp found is not at a bound.
 *
 @note This function is called only when both sequences are linear.
 */
static bool
tnumber_arithop_tp_at_timestamp1(const TInstant *start1, const TInstant *end1,
  const TInstant *start2, const TInstant *end2, TimestampTz *t)
{
  double x1 = tnumberinst_double(start1);
  double x2 = tnumberinst_double(end1);
  double x3 = tnumberinst_double(start2);
  double x4 = tnumberinst_double(end2);
  /* Compute the instants t1 and t2 at which the linear functions of the two
   * segments take the value 0: at1 + b = 0, ct2 + d = 0. There is a
   * minimum/maximum exactly at the middle between t1 and t2.
   * To reduce problems related to floating point arithmetic, t1 and t2
   * are shifted, respectively, to 0 and 1 before the computation */
  if ((x2 - x1) == 0.0 || (x4 - x3) == 0.0)
    return false;

  long double d1 = (-1 * x1) / (x2 - x1);
  long double d2 = (-1 * x3) / (x4 - x3);
  long double min = Min(d1, d2);
  long double max = Max(d1, d2);
  long double fraction = min + (max - min)/2;
  long double duration = (long double) (end1->t - start1->t);
  if (fraction <= MOBDB_EPSILON || fraction >= (1.0 - MOBDB_EPSILON))
    /* Minimum/maximum occurs out of the period */
    return false;

  *t = start1->t + (TimestampTz) (duration * fraction);
  return true;
}

/**
 * @brief Find the single timestamptz at which the operation of two temporal
 * number segments is at a local minimum/maximum.
 *
 @note This function is called only when both sequences are linear.
 */
static bool
tnumber_arithop_tp_at_timestamp(const TInstant *start1, const TInstant *end1,
  const TInstant *start2, const TInstant *end2, char op, Datum *value,
  TimestampTz *t)
{
  if (! tnumber_arithop_tp_at_timestamp1(start1, end1, start2, end2, t))
    return false;
  Datum value1 = tsegment_value_at_timestamp(start1, end1, true, *t);
  Datum value2 = tsegment_value_at_timestamp(start2, end2, true, *t);
  assert (op == '*' || op == '/');
  *value = (op == '*') ?
    datum_mult(value1, value2, start1->temptype, start2->temptype) :
    datum_div(value1, value2, start1->temptype, start2->temptype);
  return true;
}

/**
 * @brief Find the single timestamptz at which the multiplication of two temporal
 * number segments is at a local minimum/maximum.
 *
 @note This function is called only when both sequences are linear.
 */
bool
tnumber_mult_tp_at_timestamp(const TInstant *start1, const TInstant *end1,
  const TInstant *start2, const TInstant *end2, Datum *value, TimestampTz *t)
{
  return tnumber_arithop_tp_at_timestamp(start1, end1, start2, end2, '*',
    value, t);
}

/**
 * @brief Find the single timestamptz at which the division of two temporal
 * number segments is at a local minimum/maximum.
 *
 @note This function is called only when both sequences are linear.
 */
bool
tnumber_div_tp_at_timestamp(const TInstant *start1, const TInstant *end1,
  const TInstant *start2, const TInstant *end2, Datum *value, TimestampTz *t)
{
  return tnumber_arithop_tp_at_timestamp(start1, end1, start2, end2, '/',
    value, t);
}

/*****************************************************************************
 * Generic functions
 *****************************************************************************/

/**
 * @brief Generic arithmetic operator on a temporal number and a number
 * @param[in] temp Temporal number
 * @param[in] value Number
 * @param[in] basetype Base type
 * @param[in] oper Enumeration that states the arithmetic operator
 * @param[in] func Arithmetic function
 * @param[in] invert True if the base value is the first argument of the
 * function
 */
Temporal *
arithop_tnumber_number(const Temporal *temp, Datum value, meosType basetype,
  TArithmetic oper,
  Datum (*func)(Datum, Datum, meosType, meosType), bool invert)
{
  assert(tnumber_basetype(basetype));
  /* If division test whether the denominator is zero */
  if (oper == DIV)
  {
    if (invert)
    {
      if (temporal_ever_eq(temp, Float8GetDatum(0.0)))
        elog(ERROR, "Division by zero");
    }
    else
    {
      double d = datum_double(value, basetype);
      if (fabs(d) < MOBDB_EPSILON)
        elog(ERROR, "Division by zero");
    }
  }

  LiftedFunctionInfo lfinfo;
  memset(&lfinfo, 0, sizeof(LiftedFunctionInfo));
  lfinfo.func = (varfunc) func;
  lfinfo.numparam = 0;
  lfinfo.args = true;
  lfinfo.argtype[0] = temptype_basetype(temp->temptype);
  lfinfo.argtype[1] = basetype;
  lfinfo.restype = (temp->temptype == T_TINT && basetype == T_INT4) ?
    T_TINT : T_TFLOAT;
  /* This parameter is not used for tnumber <op> base */
  lfinfo.reslinear = false;
  lfinfo.invert = invert;
  lfinfo.discont = CONTINUOUS;
  lfinfo.tpfunc_base = NULL;
  lfinfo.tpfunc = NULL;
  return tfunc_temporal_base(temp, value, &lfinfo);
}

/**
 * @brief Generic arithmetic operator on two temporal numbers
 * @param[in] temp1,temp2 Temporal numbers
 * @param[in] oper Enumeration that states the arithmetic operator
 * @param[in] func Arithmetic function
 * @param[in] tpfunc Function determining the turning point
 */
Temporal *
arithop_tnumber_tnumber(const Temporal *temp1, const Temporal *temp2,
  TArithmetic oper, Datum (*func)(Datum, Datum, meosType, meosType),
  bool (*tpfunc)(const TInstant *, const TInstant *, const TInstant *,
    const TInstant *, Datum *, TimestampTz *))
{
  bool linear1 = MOBDB_FLAGS_GET_LINEAR(temp1->flags);
  bool linear2 = MOBDB_FLAGS_GET_LINEAR(temp2->flags);

  /* If division test whether the denominator will ever be zero during
   * the common timespan */
  if (oper == DIV)
  {
    SpanSet *ps = temporal_time(temp1);
    Temporal *projtemp2 = temporal_restrict_periodset(temp2, ps, REST_AT);
    if (projtemp2 == NULL)
      return NULL;
    if (temporal_ever_eq(projtemp2, Float8GetDatum(0.0)))
      elog(ERROR, "Division by zero");
  }

  LiftedFunctionInfo lfinfo;
  memset(&lfinfo, 0, sizeof(LiftedFunctionInfo));
  lfinfo.func = (varfunc) func;
  lfinfo.numparam = 0;
  lfinfo.args = true;
  lfinfo.argtype[0] = temptype_basetype(temp1->temptype);
  lfinfo.argtype[1] = temptype_basetype(temp2->temptype);
  lfinfo.restype = (temp1->temptype == T_TINT && temp2->temptype == T_TINT) ?
    T_TINT : T_TFLOAT;
  lfinfo.reslinear = linear1 || linear2;
  lfinfo.invert = INVERT_NO;
  lfinfo.discont = CONTINUOUS;
  lfinfo.tpfunc_base = NULL;
  lfinfo.tpfunc = (oper == MULT || oper == DIV) && linear1 && linear2 ?
    tpfunc : NULL;
  Temporal *result = tfunc_temporal_temporal(temp1, temp2, &lfinfo);
  return result;
}

/*****************************************************************************
 * Absolute value
 *****************************************************************************/

TInstant *
tnumberinst_abs(const TInstant *inst)
{
  meosType basetype = temptype_basetype(inst->temptype);
  assert(tnumber_basetype(basetype));
  Datum value = tinstant_value(inst);
  Datum absvalue;
  if (basetype == T_INT4)
    absvalue = Int32GetDatum(abs(DatumGetInt32(value)));
  else /* basetype == T_FLOAT8 */
    absvalue = Float8GetDatum(fabs(DatumGetFloat8(value)));
  TInstant *result = tinstant_make(absvalue, inst->temptype, inst->t);
  return result;
}

TSequence *
tnumberseq_iter_abs(const TSequence *seq)
{
  interpType interp = MOBDB_FLAGS_GET_INTERP(seq->flags);
  TInstant **instants = palloc(sizeof(TInstant *) * seq->count);
  for (int i = 0; i < seq->count; i++)
  {
    const TInstant *inst = TSEQUENCE_INST_N(seq, i);
    instants[i] = tnumberinst_abs(inst);
  }
  TSequence *result = tsequence_make_free(instants, seq->count,
    seq->period.lower_inc, seq->period.upper_inc, interp, NORMALIZE);
  return result;
}

TSequence *
tnumberseq_linear_abs(const TSequence *seq)
{
  const TInstant *inst1;
  meosType basetype = temptype_basetype(seq->temptype);

  /* Instantaneous sequence */
  if (seq->count == 1)
  {
    inst1 = TSEQUENCE_INST_N(seq, 0);
    TInstant *inst2 = tnumberinst_abs(inst1);
    TSequence *result = tinstant_to_tsequence(inst2, LINEAR);
    pfree(inst2);
    return result;
  }

  /* General case */
  TInstant **instants = palloc(sizeof(TInstant *) * seq->count * 2);
  inst1 = TSEQUENCE_INST_N(seq, 0);
  instants[0] = tnumberinst_abs(inst1);
  Datum value1 = tinstant_value(inst1);
  double dvalue1 = datum_double(value1, basetype);
  int k = 1;
  Datum dzero = (basetype == T_INT4) ? Int32GetDatum(0) : Float8GetDatum(0);
  for (int i = 1; i < seq->count; i++)
  {
    const TInstant *inst2 = TSEQUENCE_INST_N(seq, i);
    Datum value2 = tinstant_value(inst2);
    double dvalue2 = datum_double(value2, basetype);
    /* Add the instant when the segment has value equal to zero */
    if ((dvalue1 < 0 && dvalue2 > 0) || (dvalue1 > 0 && dvalue2 < 0))
    {
      double ratio = fabs(dvalue1) / (fabs(dvalue1) + fabs(dvalue2));
      double duration = (double) (inst2->t - inst1->t);
      TimestampTz t = inst1->t + (TimestampTz) (duration * ratio);
      if (t != inst1->t && t != inst2->t)
        instants[k++] = tinstant_make(dzero, seq->temptype, t);
    }
    instants[k++] = tnumberinst_abs(inst2);
    inst1 = inst2;
    value1 = value2;
    dvalue1 = dvalue2;
  }
  /* We are sure that k > 0 */
  return tsequence_make_free(instants, k, seq->period.lower_inc,
    seq->period.upper_inc, LINEAR, NORMALIZE);
}

TSequenceSet *
tnumberseqset_abs(const TSequenceSet *ss)
{
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->count);
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = TSEQUENCESET_SEQ_N(ss, i);
    sequences[i] = MOBDB_FLAGS_GET_LINEAR(ss->flags) ?
      tnumberseq_linear_abs(seq) : tnumberseq_iter_abs(seq);
  }
  TSequenceSet *result = tsequenceset_make_free(sequences, ss->count,
    NORMALIZE);
  return result;
}

/**
 * @ingroup libmeos_temporal_math
 * @brief Get the absolute value of a temporal number
 * @sqlfunc abs()
 */
Temporal *
tnumber_abs(const Temporal *temp)
{
  Temporal *result = NULL;
  assert(temptype_subtype(temp->subtype));
  if (temp->subtype == TINSTANT)
    result = (Temporal *) tnumberinst_abs((TInstant *) temp);
  else if (temp->subtype == TSEQUENCE)
    result = MOBDB_FLAGS_GET_LINEAR(temp->flags) ?
      (Temporal *) tnumberseq_linear_abs((TSequence *) temp) :
      (Temporal *) tnumberseq_iter_abs((TSequence *) temp);
  else /* temp->subtype == TSEQUENCESET */
    result = (Temporal *) tnumberseqset_abs((TSequenceSet *) temp);
  return result;
}

/*****************************************************************************
 * Delta value
 *****************************************************************************/

/**
 * @brief Return the delta value of the two numbers
 */
static Datum
delta_value(Datum value1, Datum value2, meosType basetype)
{
  Datum result;
  if (basetype == T_INT4)
    result = Int32GetDatum(DatumGetInt32(value2) - DatumGetInt32(value1));
  else /* basetype == T_FLOAT8 */
    result = Float8GetDatum(DatumGetFloat8(value2) - DatumGetFloat8(value1));
  return result;
}

/**
 * @brief Return the temporal delta value of a temporal number.
 */
static TSequence *
tnumberseq_delta_value(const TSequence *seq)
{
  /* Instantaneous sequence */
  if (seq->count == 1)
    return NULL;

  /* General case */
  /* We are sure that there are at least 2 instants */
  TInstant **instants = palloc(sizeof(TInstant *) * seq->count);
  const TInstant *inst1 = TSEQUENCE_INST_N(seq, 0);
  Datum value1 = tinstant_value(inst1);
  meosType basetype = temptype_basetype(seq->temptype);
  Datum delta = 0; /* make compiler quiet */
  for (int i = 1; i < seq->count; i++)
  {
    const TInstant *inst2 = TSEQUENCE_INST_N(seq, i);
    Datum value2 = tinstant_value(inst2);
    delta = delta_value(value1, value2, basetype);
    instants[i - 1] = tinstant_make(delta, seq->temptype, inst1->t);
    inst1 = inst2;
    value1 = value2;
  }
  instants[seq->count - 1] = tinstant_make(delta, seq->temptype, inst1->t);
  /* Resulting sequence has discrete or step interpolation */
  interpType interp = MOBDB_FLAGS_GET_DISCRETE(seq->flags) ?
    DISCRETE : STEP;
  return tsequence_make_free(instants, seq->count, seq->period.lower_inc,
    false, interp, NORMALIZE);
}

/**
 * @brief Return the temporal delta_value of a temporal number.
 */
static TSequenceSet *
tnumberseqset_delta_value(const TSequenceSet *ss)
{
  TSequence *delta;
  /* Singleton sequence set */
  if (ss->count == 1)
  {
    delta = tnumberseq_delta_value(TSEQUENCESET_SEQ_N(ss, 0));
    TSequenceSet *result = tsequence_to_tsequenceset(delta);
    pfree(delta);
    return result;
  }

  /* General case */
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->count);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = TSEQUENCESET_SEQ_N(ss, i);
    delta = tnumberseq_delta_value(seq);
    if (delta)
      sequences[k++] = delta;
  }
  if (k == 0)
  {
    pfree(sequences);
    return NULL;
  }
  /* Resulting sequence set has step interpolation */
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/**
 * @ingroup mobilitydb_temporal_math
 * @brief Return the delta value of a temporal number.
 * @sqlfunc deltaValue()
 */
Temporal *
tnumber_delta_value(const Temporal *temp)
{
  Temporal *result = NULL;
  assert(temptype_subtype(temp->subtype));
  if (temp->subtype == TINSTANT)
    ;
  else if (temp->subtype == TSEQUENCE)
    result = (Temporal *) tnumberseq_delta_value((TSequence *) temp);
  else /* temp->subtype == TSEQUENCESET */
    result = (Temporal *) tnumberseqset_delta_value((TSequenceSet *) temp);
  return result;
}



/*****************************************************************************
 * Speed
 *****************************************************************************/



double speed(const TInstant *start, const TInstant *end, bool hasz)
{
  double distance = 0;

  if (hasz)
  {
    POINT3DZ valueinst1, valueinst2;
    valueinst1  = datum_point3dz(tinstant_value(start));
    valueinst2  = datum_point3dz(tinstant_value(start));
    distance = distance3d_pt_pt(&valueinst1, &valueinst2);
  }
  else 
  {
    POINT2D valueinst12D, valueinst22D;
    valueinst12D = datum_point2d(tinstant_value(start));
    valueinst22D = datum_point2d(tinstant_value(end));
    distance = distance2d_pt_pt(&valueinst12D, &valueinst22D);  
  }

  double totaltime = ((double) end->t - (double) start->t)/10000;

  double speedNow = distance /totaltime;
  /* a = Δv/Δt. */
  return speedNow;
}


double distance(const TInstant *start, const TInstant *end, bool hasz)
{
  double distance = 0;

  if (hasz)
  {
    POINT3DZ valueinst1, valueinst2;
    valueinst1  = datum_point3dz(tinstant_value(start));
    valueinst2  = datum_point3dz(tinstant_value(start));
    distance = distance3d_pt_pt(&valueinst1, &valueinst2);
  }
  else 
  {
    POINT2D valueinst12D, valueinst22D;
    valueinst12D = datum_point2d(tinstant_value(start));
    valueinst22D = datum_point2d(tinstant_value(end));
    distance = distance2d_pt_pt(&valueinst12D, &valueinst22D);  
  }
  /* a = Δv/Δt. */
  return distance;
}

double cumulative_distance(const TSequence* seq)
{

  /* Do not try to detect speed on really short things */
  if (seq->count < 2)
    return 0;

  bool hasz = MOBDB_FLAGS_GET_Z(seq->flags);
  
  const TInstant *inst1 = TSEQUENCE_INST_N(seq, 0);
  double cumulativeDistance = 0;

  for (int i=1; i < seq->count; i++)
  {
    const TInstant *inst2 = TSEQUENCE_INST_N(seq, i);
    cumulativeDistance += distance(inst1, inst2, hasz);
    inst1 = inst2;
  }
  return cumulativeDistance;
}


double tsequence_max_speed(const TSequence* seq)
{

  /* Do not try to detect speed on really short things */
  if (seq->count < 2)
    return 0;

  bool hasz = MOBDB_FLAGS_GET_Z(seq->flags);
  
  const TInstant *inst1 = TSEQUENCE_INST_N(seq, 0);
  double maxSpeed = 0;

  for (int i=1; i < seq->count; i++)
  {
    const TInstant *inst2 = TSEQUENCE_INST_N(seq, i);
    double speedNow = speed(inst1, inst2, hasz);
    if ((speedNow > maxSpeed))
    {
      if (speedNow < 1000)
        maxSpeed = speedNow;
      else
        maxSpeed = 1000;
    }
    inst1 = inst2;
  }
  return maxSpeed;
}

double average_speed(const TSequence* seq)
{

  /* Do not try to detect speed on really short things */
  if (seq->count < 2)
    return 0;

  bool hasz = MOBDB_FLAGS_GET_Z(seq->flags);
  
  const TInstant *inst1 = TSEQUENCE_INST_N(seq, 0);
  double cumulativeSpeed = 0;

  for (int i=1; i < seq->count; i++)
  {
    const TInstant *inst2 = TSEQUENCE_INST_N(seq, i);
    double speedNow = speed(inst1, inst2, hasz);
    cumulativeSpeed += speedNow;
    inst1 = inst2;
  }
  return cumulativeSpeed/seq->count;
}


double * tsequenceset_max_speed(TSequenceSet *ss)
{
  double result[ss->count];

  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = TSEQUENCE_INST_N(ss, i);
    result[i] = tsequence_max_speed(seq);
  }
  return result;
}

 /**
 * @ingroup filter outlier
 * @brief Detect outliers in a the temporal type using a heuritic
 * based on algorithm. based on speed
 * @sqlfunc outlierSpeed
 */
Temporal * temporal_maxSpeed(Temporal *temp)
{
  double resultSpeed = 0;
  double * resultSpeedSet = NULL;
  
  if (temp->subtype == TINSTANT || ! MOBDB_FLAGS_GET_LINEAR(temp->flags))
    resultSpeed = 0;

  else if (temp->subtype == TSEQUENCE)
    resultSpeed = tsequence_max_speed((TSequence *) temp);

  else /* temp->subtype == TSEQUENCESET */
    resultSpeedSet = tsequenceset_max_speed((TSequenceSet *) temp);

  return NULL;
}





/*****************************************************************************
 * Angular difference
 *****************************************************************************/

/**
 * @brief Return the angular difference, i.e., the smaller angle between the
 * two degree values
 */
Datum
angular_difference(Datum degrees1, Datum degrees2)
{
  double diff = fabs(DatumGetFloat8(degrees1) - DatumGetFloat8(degrees2));
  if (diff > 180)
   diff = fabs(diff - 360);
  return Float8GetDatum(diff);
}

/**
 * @brief Return the temporal angular difference of a temporal number.
 * @brief This functions
 */
static int
tnumberseq_angular_difference1(const TSequence *seq, TInstant **result)
{
  /* Instantaneous sequence */
  if (seq->count == 1)
    return 0;

  /* General case */
  const TInstant *inst1 = TSEQUENCE_INST_N(seq, 0);
  Datum value1 = tinstant_value(inst1);
  Datum angdiff = Float8GetDatum(0);
  int k = 0;
  if (seq->period.lower_inc)
    result[k++] = tinstant_make(angdiff, seq->temptype, inst1->t);
  for (int i = 1; i < seq->count; i++)
  {
    const TInstant *inst2 = TSEQUENCE_INST_N(seq, i);
    Datum value2 = tinstant_value(inst2);
    angdiff = angular_difference(value1, value2);
    if (i != seq->count - 1 || seq->period.upper_inc)
      result[k++] = tinstant_make(angdiff, seq->temptype, inst2->t);
    inst1 = inst2;
    value1 = value2;
  }
  return k;
}



bool notInList(const TInstant **instants, const TInstant *inst){
  for (int i = 0; i < 4; i++)
  {
    if (instants[i] == inst)
      return false;
  }
  return true;
}

bool NoTurns(TSequence *fixedorder)
{
  // get azimuth angles
  const TSequenceSet* angulos = tpointseq_azimuth(fixedorder);
  Datum angdiff = Float8GetDatum(0);
  Datum angdiff2 = Float8GetDatum(0);

  const TInstant *inst1Angulo = TSEQUENCE_INST_N(angulos, 0);
  Datum value1Angulo = tinstant_value(inst1Angulo);
  const TInstant *inst2Angulo = TSEQUENCE_INST_N(angulos, 1);
  Datum value2Angulo = tinstant_value(inst2Angulo);

  for (int i = 2; i < fixedorder->count; i++)
  {
    const TInstant *inst3Angulo = TSEQUENCE_INST_N(angulos, i);
    Datum value3Angulo = tinstant_value(inst3Angulo);
    //check angles 3 by tree
    angdiff = angular_difference(value1Angulo, value2Angulo);
    angdiff2 = angular_difference(value2Angulo, value3Angulo);

    if (angdiff > 120 && angdiff2 > 120)
      return false;

    inst1Angulo = inst2Angulo;
    value1Angulo = value2Angulo;
    inst2Angulo = inst3Angulo;
    value2Angulo = value3Angulo;

  }
  return true;
}



void fixOutOfOrder( TSequence *originalseq, int *positions, int positionsSize ,TSequence *fixedorder)
{
  int k = 0;
  const TInstant **instants = palloc(sizeof(TInstant *) * 4);
  /* TODO if point is at list of out-of-order, change based on angle as brute force */
  for (int i = 0; i < positionsSize; i++)
  {
    /*find where to put the point that is out of order,
      while not best match continue changing points */
    for (int j = 0; j < originalseq->count; j++)
    {
      const TInstant *inst = TSEQUENCE_INST_N(originalseq, positions[i]);
      fixedorder = tsequence_make(instants, j, true, true, DISCRETE, NORMALIZE);
      // if no turning angles anymore, break
      if (NoTurns(fixedorder))
        break;
    }
  }
}


/**
 * @brief Return the temporal number if the angular difference between three poits is less than 180
 * @brief This functions
 */
static int
tnumberseq_angular_difference3(const TSequence *seq, TSequence **result,TSequence *originalseq)
{
  //char *seq1_wkt = tpoint_as_ewkt((Temporal *) originalseq, 2);
  //elog(INFO, "seql: %s\n", seq1_wkt);
 
  /* Small sequence */
  if (seq->count < 3)
    return seq->count;
  
  /* General case */
  int positions[seq->count];

  const TInstant **instants = palloc(sizeof(TInstant *) * 4);

  const TInstant *inst1Angulo = TSEQUENCE_INST_N(seq, 0);
  const TInstant *inst1 = TSEQUENCE_INST_N(originalseq, 0);

  Datum value1Angulo = tinstant_value(inst1Angulo);
  Datum angdiff = Float8GetDatum(0);
  Datum angdiff2 = Float8GetDatum(0);
  int k = 0;
  int l = 0;

  const TInstant *inst2Angulo = TSEQUENCE_INST_N(seq, 1);
  const TInstant *inst2 = TSEQUENCE_INST_N(originalseq, 1);
  Datum value2Angulo = tinstant_value(inst2Angulo);

  
  /* check angular difference between first and second point, then second and third point
  if the difference is greater than 120 in both cases, then the point is a turning point */
  
  for (int i = 2; i < seq->count; i++)
  {
    const TInstant *inst3Angulo = TSEQUENCE_INST_N(seq, i);
    const TInstant *inst3 = TSEQUENCE_INST_N(originalseq, i);

    Datum value3Angulo = tinstant_value(inst3Angulo);

    angdiff = angular_difference(value1Angulo, value2Angulo);
    angdiff2 = angular_difference(value2Angulo, value3Angulo);

    if (angdiff > 120 && angdiff2 > 120)
    {
      int j = 0;
      // char *seq1_wkt2 = tpoint_as_ewkt((Temporal *) inst1, 2);
      // char *seq2_wkt2 = tpoint_as_ewkt((Temporal *) inst2, 2);
      // char *seq3_wkt2 = tpoint_as_ewkt((Temporal *) inst3, 2);

      /* If point is already in the list, do not add it */
      if (notInList(instants,inst1))
      {
        instants[j++]=inst1;
        positions[l++] = i-2;
      }
      if (notInList(instants,inst2))
      {
        instants[j++]=inst2;
        positions[l++] = i-1;
      }
      if (notInList(instants,inst3))
      {
        instants[j++]=inst3;
        positions[l++] = i;
      }

      /* if point is already in the list, do not add it */
      result[k++]= tsequence_make(instants, j, true, true, DISCRETE, NORMALIZE);
    }
    /* Advance in sliding window */
    inst1 = inst2;
    inst1Angulo = inst2Angulo;
    value1Angulo = value2Angulo;
    inst2 = inst3;
    inst2Angulo = inst3Angulo;
    value2Angulo = value3Angulo;
  }
  TSequence *fixedorder = NULL;

  fixOutOfOrder(originalseq, positions, l, fixedorder);
  //tsequenceset_to_tsequence
  return k;
}


static TSequence *
tnumberseq_angular_difference(const TSequence *seq)
{
  /* Instantaneous sequence */
  if (seq->count == 1)
    return NULL;

  /* General case */
  /* We are sure that there are at least 2 instants */
  TInstant **instants = palloc(sizeof(TInstant *) * seq->count);
  int k = tnumberseq_angular_difference1(seq, instants);
  if (k == 0)
    return NULL;
  /* Resulting sequence has discrete interpolation */
  return tsequence_make_free(instants, k, true, true, DISCRETE, NORMALIZE);
}

/**
 * @brief Return the temporal delta_value of a temporal number.
 */
static TSequence *
tnumberseqset_angular_difference(const TSequenceSet *ss)
{
  /* Singleton sequence set */
  if (ss->count == 1)
    return tnumberseqset_angular_difference(TSEQUENCESET_SEQ_N(ss, 0));

  /* General case */
  TInstant **instants = palloc(sizeof(TSequence *) * ss->totalcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = TSEQUENCESET_SEQ_N(ss, i);
    k += tnumberseq_angular_difference1(seq, &instants[k]);
  }
  if (k == 0)
    return NULL;
  /* Resulting sequence has discrete interpolation */
  return tsequence_make_free(instants, k, true, true, DISCRETE, NORMALIZE);
}

/**
 * @ingroup mobilitydb_temporal_math
 * @brief Return the angular difference of a temporal number.
 * @sqlfunc angularDifference()
 */
Temporal *
tnumber_angular_difference(const Temporal *temp)
{
  Temporal *result = NULL;
  assert(temptype_subtype(temp->subtype));
  if (temp->subtype == TINSTANT)
    ;
  else if (temp->subtype == TSEQUENCE)
    result = (Temporal *) tnumberseq_angular_difference((TSequence *) temp);
  else /* temp->subtype == TSEQUENCESET */
    result = (Temporal *) tnumberseqset_angular_difference((TSequenceSet *) temp);
  return result;
}

static TSequenceSet *
tnumberseq_angular_difference_3points(const TSequence *seq,TSequence *originalseq)
{
  /* Instantaneous sequence */
  if (seq->count == 1)
    return NULL;

  /* General case */
  /* We are sure that there are at least 2 instants */
  TSequence **sequences = palloc(sizeof(TSequence *) * seq->count);
  int k = tnumberseq_angular_difference3(seq, sequences,originalseq);
  if (k == 0)
    return NULL;

  /* Resulting sequence has discrete interpolation */
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}
/**
 * @brief Return the temporal delta_value of a temporal number.
 */
static TSequenceSet *
tnumberseqset_angular_difference_3points(const TSequenceSet *ss,TSequence *originalseq)
{
  /* Singleton sequence set */
  if (ss->count == 1)
    return tnumberseq_angular_difference_3points(TSEQUENCESET_SEQ_N(ss, 0),originalseq);

  /* General case */
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->totalcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = TSEQUENCESET_SEQ_N(ss, i);
    TSequence **temp;
    int j= tnumberseq_angular_difference3(seq, temp,originalseq);
  }
   if (k == 0)
  {
    pfree(sequences);
    return NULL;
  }
  /* Resulting sequence has discrete interpolation */
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}


/**
 * @ingroup mobilitydb_temporal_math
 * @brief Return the angular difference of a temporal number.
 * @sqlfunc angularDifference()
 */
Temporal *
tnumber_angular_difference_3points(const Temporal *temp,const Temporal *seq)
{
  Temporal *result = NULL;
  assert(temptype_subtype(temp->subtype));

  if (temp->subtype == TINSTANT);
  else if (temp->subtype == TSEQUENCE)
    result = (Temporal *) tnumberseq_angular_difference_3points((TSequenceSet *) temp,(TSequence *)seq);
  else /* temp->subtype == TSEQUENCESET */
    result = (Temporal *) tnumberseqset_angular_difference_3points((TSequenceSet *) temp,(TSequenceSet *)seq);

  return result;
}



/*****************************************************************************
 * Miscellaneous temporal functions
 *****************************************************************************/

/**
 * @ingroup libmeos_temporal_math
 * @brief Convert a temporal number from radians to degrees
 * @sqlfunc degrees()
 */
Temporal *
tfloat_degrees(const Temporal *temp, bool normalize)
{
  /* We only need to fill these parameters for tfunc_temporal */
  LiftedFunctionInfo lfinfo;
  memset(&lfinfo, 0, sizeof(LiftedFunctionInfo));
  lfinfo.func = (varfunc) &datum_degrees;
  lfinfo.numparam = 1;
  lfinfo.param[0] = BoolGetDatum(normalize);
  lfinfo.args = true;
  lfinfo.argtype[0] = temptype_basetype(temp->temptype);
  lfinfo.restype = T_TFLOAT;
  lfinfo.tpfunc_base = NULL;
  lfinfo.tpfunc = NULL;
  Temporal *result = tfunc_temporal(temp, &lfinfo);
  return result;
}

/**
 * @ingroup libmeos_temporal_math
 * @brief Convert a temporal number from degrees to radians
 * @sqlfunc radians()
 */
Temporal *
tfloat_radians(const Temporal *temp)
{
  /* We only need to fill these parameters for tfunc_temporal */
  LiftedFunctionInfo lfinfo;
  memset(&lfinfo, 0, sizeof(LiftedFunctionInfo));
  lfinfo.func = (varfunc) &datum_radians;
  lfinfo.numparam = 0;
  lfinfo.args = true;
  lfinfo.argtype[0] = temptype_basetype(temp->temptype);
  lfinfo.restype = T_TFLOAT;
  lfinfo.tpfunc_base = NULL;
  lfinfo.tpfunc = NULL;
  Temporal *result = tfunc_temporal(temp, &lfinfo);
  return result;
}

/*****************************************************************************
 * Derivative functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_math
 * @brief Return the derivative of a temporal sequence number.
 * @sqlfunc derivative()
 */
TSequence *
tfloatseq_derivative(const TSequence *seq)
{
  assert(MOBDB_FLAGS_GET_LINEAR(seq->flags));

  /* Instantaneous sequence */
  if (seq->count == 1)
    return NULL;

  /* General case */
  meosType basetype = temptype_basetype(seq->temptype);
  TInstant **instants = palloc(sizeof(TInstant *) * seq->count);
  const TInstant *inst1 = TSEQUENCE_INST_N(seq, 0);
  Datum value1 = tinstant_value(inst1);
  double dvalue1 = datum_double(value1, basetype);
  double derivative = 0.0; /* make compiler quiet */
  for (int i = 0; i < seq->count - 1; i++)
  {
    const TInstant *inst2 = TSEQUENCE_INST_N(seq, i + 1);
    Datum value2 = tinstant_value(inst2);
    double dvalue2 = datum_double(value2, basetype);
    derivative = datum_eq(value1, value2, basetype) ? 0.0 :
      (dvalue1 - dvalue2) / ((double)(inst2->t - inst1->t) / 1000000);
    instants[i] = tinstant_make(Float8GetDatum(derivative), T_TFLOAT, inst1->t);
    inst1 = inst2;
    value1 = value2;
    dvalue1 = dvalue2;
  }
  instants[seq->count - 1] = tinstant_make(Float8GetDatum(derivative),
    T_TFLOAT, seq->period.upper);
  /* The resulting sequence has step interpolation */
  TSequence *result = tsequence_make((const TInstant **) instants, seq->count,
    seq->period.lower_inc, seq->period.upper_inc, STEP, NORMALIZE);
  pfree_array((void **) instants, seq->count - 1);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_math
 * @brief Return the derivative of a temporal sequence set number
 * @sqlfunc derivative()
 */
TSequenceSet *
tfloatseqset_derivative(const TSequenceSet *ss)
{
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->count);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = TSEQUENCESET_SEQ_N(ss, i);
    if (seq->count > 1)
      sequences[k++] = tfloatseq_derivative(seq);
  }
  if (k == 0)
  {
    pfree(sequences);
    return NULL;
  }
  /* The resulting sequence set has step interpolation */
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/**
 * @ingroup libmeos_temporal_math
 * @brief Return the derivative of a temporal number
 * @see tfloatseq_derivative
 * @see tfloatseqset_derivative
 * @sqlfunc derivative()
 */
Temporal *
tfloat_derivative(const Temporal *temp)
{
  Temporal *result = NULL;
  assert(temptype_subtype(temp->subtype));
  if (temp->subtype == TINSTANT || ! MOBDB_FLAGS_GET_LINEAR(temp->flags))
    ;
  else if (temp->subtype == TSEQUENCE)
    result = (Temporal *) tfloatseq_derivative((TSequence *) temp);
  else /* temp->subtype == TSEQUENCESET */
    result = (Temporal *) tfloatseqset_derivative((TSequenceSet *) temp);
  return result;
}

/*****************************************************************************/
