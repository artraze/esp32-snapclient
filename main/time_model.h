#include <stdint.h>

#define XTIME_MODEL_SCALE_FRAC              27

#define XTIME_SAMPLES_MAX_RAW               24   // Max size of the input sample buffer
#define XTIME_SAMPLES_MAX_FILT              24   // Max size of the filtered sample buffer
#define XTIME_SAMPLES_CALC_INITIAL          16   // Number of samples used to do initial estimate 
#define XTIME_SAMPLES_CALC_FILT              8    // Number of samples collected before adding a filtered point
#define XTIME_SAMPLES_CALC_INITIAL_ERR    -200

typedef struct TimeModelBase
{
	uint64_t epoch_time;
	uint64_t clk_offset;
	uint32_t clk_us_scale;
} TimeModelBase;

typedef struct TimeModel
{
	TimeModelBase m1;
	TimeModelBase m2;
	uint32_t clk_us_scale_x;
} TimeModel;

typedef struct TimeModelState
{
	uint32_t sync_status;
	TimeModel model;
	
	uint32_t n_samples_filt;
	uint64_t ntp_samples_filt[XTIME_SAMPLES_MAX_FILT];
	uint64_t clk_samples_filt[XTIME_SAMPLES_MAX_FILT];

	uint32_t n_samples_raw;
	uint64_t ntp_samples_raw[XTIME_SAMPLES_MAX_RAW];
	uint64_t clk_samples_raw[XTIME_SAMPLES_MAX_RAW];
} TimeModelState;

uint64_t xtime_calc(const TimeModel *model, uint64_t clk);

int xtime_add_observation(TimeModelState *state, uint64_t ntp, uint64_t sys);
