#!/usr/bin/env python3
from pathlib import Path

p = Path('twinvq/src/twinvq_encoder.cpp')
s = p.read_text()
old = '''    constexpr int spectral_bins = 128;
    float target_log[spectral_bins]{}, grid[spectral_bins]{};
    if (search == LspSearch::Spectral) {
        float lsp_cos[kLspCoefsMax];
        for (int j = 0; j < order; ++j) lsp_cos[j] = 2.0f * std::cos(target_lsp[j]);
        for (int k = 0; k < spectral_bins; ++k) {
            const float position = (k + 0.5f) / spectral_bins;
            if (cfg_.sibilant_protection) {
                // Resolve narrow low/mid-frequency envelope structure more
                // densely while retaining coverage through Nyquist.
                const double nyquist = sample_rate_ * 0.5;
                const double hz = 600.0 * std::expm1(position * std::log1p(nyquist / 600.0));
                grid[k] = static_cast<float>(std::cos(kPi * hz / nyquist));
            } else {
                grid[k] = std::cos(kPi * (k + 0.5f) / spectral_bins);
            }
            target_log[k] = std::log(std::clamp(
                eval_lpc_spectrum(lsp_cos, grid[k], order), 1.0e-20f, 1.0e20f));
        }
    }
    auto lsp_error = [&](const float* rec) {
        if (search != LspSearch::Spectral) return vec_err(target_lsp, rec, order);
        float lsp_cos[kLspCoefsMax];
        for (int j = 0; j < order; ++j) lsp_cos[j] = 2.0f * std::cos(rec[j]);
        double sum = 0, squares = 0;
        for (int k = 0; k < spectral_bins; ++k) {
            const double d = std::log(std::clamp(
                eval_lpc_spectrum(lsp_cos, grid[k], order), 1.0e-20f, 1.0e20f)) - target_log[k];
            sum += d; squares += d*d;
        }
        return static_cast<float>(std::max(0.0, squares - sum*sum/spectral_bins));
    };
'''
new = '''    constexpr int spectral_bins = 128;
    float target_log[spectral_bins]{}, grid[spectral_bins]{}, grid_weight[spectral_bins]{};
    if (search == LspSearch::Spectral) {
        float lsp_cos[kLspCoefsMax];
        for (int j = 0; j < order; ++j) lsp_cos[j] = 2.0f * std::cos(target_lsp[j]);
        for (int k = 0; k < spectral_bins; ++k) {
            const float position = (k + 0.5f) / spectral_bins;
            grid_weight[k] = 1.0f;
            if (cfg_.sibilant_protection) {
                // Resolve narrow low/mid-frequency envelope structure more
                // densely while retaining coverage through Nyquist.
                const double nyquist = sample_rate_ * 0.5;
                const double hz = 600.0 * std::expm1(position * std::log1p(nyquist / 600.0));
                grid[k] = static_cast<float>(std::cos(kPi * hz / nyquist));
                // Speech timbre is dominated by formant placement. Give the
                // spectral LSP candidate a modest extra preference in the
                // 300-4000 Hz region while retaining the full-band objective.
                const double rise = std::clamp((hz - 250.0) / 500.0, 0.0, 1.0);
                const double fall = std::clamp((4500.0 - hz) / 1500.0, 0.0, 1.0);
                grid_weight[k] = static_cast<float>(1.0 + 0.5 * rise * fall);
            } else {
                grid[k] = std::cos(kPi * (k + 0.5f) / spectral_bins);
            }
            target_log[k] = std::log(std::clamp(
                eval_lpc_spectrum(lsp_cos, grid[k], order), 1.0e-20f, 1.0e20f));
        }
    }
    auto lsp_error = [&](const float* rec) {
        if (search != LspSearch::Spectral) return vec_err(target_lsp, rec, order);
        float lsp_cos[kLspCoefsMax];
        for (int j = 0; j < order; ++j) lsp_cos[j] = 2.0f * std::cos(rec[j]);
        double sum = 0, squares = 0, weight_sum = 0;
        for (int k = 0; k < spectral_bins; ++k) {
            const double d = std::log(std::clamp(
                eval_lpc_spectrum(lsp_cos, grid[k], order), 1.0e-20f, 1.0e20f)) - target_log[k];
            const double w = grid_weight[k];
            sum += w * d;
            squares += w * d*d;
            weight_sum += w;
        }
        return static_cast<float>(std::max(0.0, squares - sum*sum/weight_sum));
    };
'''

if old not in s:
    raise SystemExit('expected LSP block not found')
p.write_text(s.replace(old, new, 1))
print('applied LSP voice-formant weighting experiment')
