// Logging is _absurdly_ slow.  Like xtime_add_observation takes 1us to add the sample, 800us to
// print "ADDING SAMPLE ...".  So disable the info logs unless this is getting tested
#define LOG_LOCAL_LEVEL ESP_LOG_WARN

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "time_model.h"
#include "esp_log.h"
#include "esp_attr.h"

static const char *TAG = "app_time";

IRAM_ATTR uint64_t xtime_calc_base(const TimeModelBase *model, uint64_t clk)
{
	// This could round, but really the 1us resolution is an order of magnitude less error than this
	// could ever hope for.
	return model->epoch_time + ((model->clk_us_scale * ((int64_t)(clk - model->clk_offset))) >> XTIME_MODEL_SCALE_FRAC);
}

IRAM_ATTR uint64_t xtime_calc(const TimeModel *model, uint64_t clk)
{
	int64_t clk1 = clk - model->m1.clk_offset;
	int64_t clk2 = clk - model->m2.clk_offset;
	if (clk1 < 0)
	{
		return model->m1.epoch_time + ((model->m1.clk_us_scale * clk1) >> XTIME_MODEL_SCALE_FRAC);
	}
	else if (clk2 > 0)
	{
		return model->m2.epoch_time + ((model->m2.clk_us_scale * clk2) >> XTIME_MODEL_SCALE_FRAC);
	}
	else
	{
		return model->m1.epoch_time + ((model->clk_us_scale_x * clk1) >> XTIME_MODEL_SCALE_FRAC);
	}
}

void xtime_model_estimate_linreg(const uint64_t *clk, const uint64_t *ntp, uint32_t nsamples, TimeModelBase *model)
{
	uint64_t sum_ntp     = 0;
	uint64_t sum_clk     = 0;
	uint64_t sum_clk_clk = 0;
	uint64_t sum_clk_ntp = 0;
	uint32_t count = 1;
	// offset = (np.sum(y) * np.sum(x*x) - np.sum(x)*np.sum(x*y)) / (n*np.sum(x*x) - np.sum(x)**2)
	// slope  = (n*np.sum(x*y) - np.sum(x)*np.sum(y)) / (n*np.sum(x*x) - np.sum(x)**2)
	for (uint32_t i = 1; i < nsamples; i++)
	{
		uint64_t v_ntp = ntp[i] - ntp[0];
		uint64_t v_clk = ((clk[i] - clk[0]) + 8) >> 4;
		sum_ntp     += v_ntp;
		sum_clk     += v_clk;
		sum_clk_clk += v_clk*v_clk;
		sum_clk_ntp += v_clk*v_ntp;
		count++;
	}
	// Sadly this kind of needs to be floats since the X^2 terms are pushing 64b and there isn't 
	// really an easy way to predict how to split out the divisors Z so that Y*X^2/Z doesn't 
	// overflow.
	double denom = 1 / ((double)sum_clk_clk * (double)count - (double)sum_clk * (double)sum_clk);
	double clk_m = ((double)count * sum_clk_ntp - (double)sum_ntp * sum_clk) * denom;
	double clk_b = ((double)sum_ntp * sum_clk_clk - (double)sum_clk * sum_clk_ntp) * denom;
	
	model->epoch_time   = ntp[0] + llround(clk_b);
	model->clk_us_scale = lround((1UL<<(XTIME_MODEL_SCALE_FRAC - 4)) * clk_m);
	model->clk_offset   = clk[0];
}

int xtime_model_estimate_inital(uint64_t *clk, uint64_t *ntp, uint32_t *nsamples, TimeModelBase *model)
{
	// For initial sync, use linear regression.  Because roundtrip compensation is disabled, error/
	// noise is strictly negative so drop those and attempt to converge to a decent number of
	// points with low error.
	xtime_model_estimate_linreg(clk, ntp, *nsamples, model);
	ESP_LOGI(TAG, "Initial model estimate epoch_time = %lli, clk_offset = %lli, clk_us_scale = %li", model->epoch_time, model->clk_offset, model->clk_us_scale);
		
	// Trim points.  Rather than calc the mean square error and filter on that, this will
	// a more aggressive approach of just dropping anything that shows more than a slight 
	// negative error.
	uint32_t n = 0;
	for (uint32_t i = 0; i < *nsamples; i++)
	{
		int32_t err = ntp[i] - xtime_calc_base(model, clk[i]);
		if (err < XTIME_SAMPLES_CALC_INITIAL_ERR)
		{
			ESP_LOGI(TAG, "Dropping sample (err=%i)", err);
			continue;
		}
		if (n != i)
		{
			clk[n] = clk[i];
			ntp[n] = ntp[i];
		}
		n++;
	}
	ESP_LOGI(TAG, "Filtering left %i points", n);
	
	// If no points were removed, then this model is considered correct.  Otherwise, return that it
	// failed and try again once more points have been collected.
	if (n == *nsamples)
	{
		return 1;
	}
	*nsamples = n;
	return 0;
}

int xtime_add_observation(TimeModelState *state, uint64_t ntp, uint64_t sys)
{
	ESP_LOGI(TAG, "ADDING SAMPLE sys=%lli, ntp=%lli", sys, ntp);
	
	// Add the raw sample to the buffer
	if (state->n_samples_raw < XTIME_SAMPLES_MAX_RAW)
	{
		state->ntp_samples_raw[state->n_samples_raw] = ntp;
		state->clk_samples_raw[state->n_samples_raw] = sys;
		state->n_samples_raw++;
	}

	if (state->sync_status == 0)
	{
		if (state->n_samples_raw < XTIME_SAMPLES_CALC_INITIAL)
		{
			return 0;
		}
	
		TimeModelBase model;
		if (!xtime_model_estimate_inital(state->clk_samples_raw, state->ntp_samples_raw, &state->n_samples_raw, &model))
		{
			return 0;
		}
		// The model checked out so init the actual model with it
		state->model.m1 = model;
		state->model.m2 = model;
		state->model.clk_us_scale_x = model.clk_us_scale;
		state->sync_status = 1;
		ESP_LOGI(TAG, "Init model epoch_time = %lli, clk_offset = %lli, clk_us_scale = %li", 
						state->model.m1.epoch_time, state->model.m1.clk_offset, state->model.m1.clk_us_scale);
		// At this point the raw samples have been filtered to match the initial model fairly well
		// so use them to fill the initial filtered buffer.
		memcpy(state->ntp_samples_filt, state->ntp_samples_raw, state->n_samples_raw*sizeof(uint64_t));
		memcpy(state->clk_samples_filt, state->clk_samples_raw, state->n_samples_raw*sizeof(uint64_t));
		state->n_samples_filt = state->n_samples_raw;
		state->n_samples_raw  = 0;
		return 1;
	}
	
	if (state->n_samples_raw < XTIME_SAMPLES_CALC_FILT)
	{
		return 0;
	}
	
	// With enough points in the RAW buffer, find the one with the least error (which in this
	// context means the numerically largest) to add to the filtered buffer.  
	// TODO: Should this handle jumps better?  It'll take a fair bit of time for the average to
	// pull without some explicit reset, but maybe that's not too bad.  It could let filtering
	// noise be more aggressive though.
	int32_t min_err_v = INT32_MIN;
	int32_t min_err_i = 0;
	for (uint32_t i = 0; i < state->n_samples_raw; i++)
	{
		int32_t err = state->ntp_samples_raw[i] - xtime_calc_base(&state->model.m2, state->clk_samples_raw[i]);
		if (err > min_err_v)
		{
			min_err_v = err;
			min_err_i = i;
		}
	}

	if (state->n_samples_filt < XTIME_SAMPLES_MAX_FILT)
	{
		state->ntp_samples_filt[state->n_samples_filt] = state->ntp_samples_raw[min_err_i];
		state->clk_samples_filt[state->n_samples_filt] = state->clk_samples_raw[min_err_i];
		state->n_samples_filt++;
	}
	else
	{
		memmove(state->ntp_samples_filt, state->ntp_samples_filt + 1, (XTIME_SAMPLES_MAX_FILT-1)*sizeof(uint64_t));
		memmove(state->clk_samples_filt, state->clk_samples_filt + 1, (XTIME_SAMPLES_MAX_FILT-1)*sizeof(uint64_t));
		state->ntp_samples_filt[XTIME_SAMPLES_MAX_FILT-1] = state->ntp_samples_raw[min_err_i];;
		state->clk_samples_filt[XTIME_SAMPLES_MAX_FILT-1] = state->clk_samples_raw[min_err_i];;
	}
	
	state->n_samples_raw = 0;

	TimeModelBase model;

	xtime_model_estimate_linreg(state->clk_samples_filt, state->ntp_samples_filt, state->n_samples_filt, &model);
	ESP_LOGI(TAG, "Updated model epoch_time = %lli, clk_offset = %lli, clk_us_scale = %li", model.epoch_time, model.clk_offset, model.clk_us_scale);
	
	// TODO: This offset should probably depend on how large the time delta is, but for
	// now just set the switchover target to 100ms in the future.  Use the nominal 16MHz
	// for this (which further hard-codes the assumption the input is 16Mhz-ish).
	uint64_t now = xtime_calc(&state->model, sys);
	uint64_t adj_time = 16000LL * 60000;
	state->model.m1.clk_us_scale  = ((now - state->model.m1.epoch_time) << XTIME_MODEL_SCALE_FRAC) / (sys - state->model.m1.clk_offset);
	state->model.m1.epoch_time    = now;
	state->model.m1.clk_offset    = sys;
	state->model.m2.epoch_time    = xtime_calc_base(&model, sys + adj_time);
	state->model.m2.clk_offset    = sys + adj_time;
	state->model.m2.clk_us_scale  = model.clk_us_scale;
	
	// Calculate the fake clock required to interpolate between the two
	state->model.clk_us_scale_x = ((state->model.m2.epoch_time - state->model.m1.epoch_time) << XTIME_MODEL_SCALE_FRAC) / (state->model.m2.clk_offset - state->model.m1.clk_offset);
	
	ESP_LOGI(TAG, "Model 1 epoch_time = %lli, clk_offset = %lli, clk_us_scale = %li", 
					state->model.m1.epoch_time, state->model.m1.clk_offset, state->model.m1.clk_us_scale);
	ESP_LOGI(TAG, "Model 2 epoch_time = %lli, clk_offset = %lli, clk_us_scale = %li", 
					state->model.m2.epoch_time, state->model.m2.clk_offset, state->model.m2.clk_us_scale);
	ESP_LOGI(TAG, "Model slew scale = %li", state->model.clk_us_scale_x);
	ESP_LOGI(TAG, "DIFFS: epoch = %llims, offset = %llims", (state->model.m2.epoch_time - state->model.m1.epoch_time)/1000, (((state->model.m2.clk_offset - state->model.m1.clk_offset) * state->model.m2.clk_us_scale) >> XTIME_MODEL_SCALE_FRAC)/1000);

	return 1;
}
