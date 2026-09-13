#!/usr/bin/env python3
from pathlib import Path

path = Path("twinvq/src/twinvq_encoder.cpp")
text = path.read_text()
old = """        for (int refinement = 0; refinement < 2; ++refinement) {
            bool improved = false;
            for (int part = 0; part < mtab_->lsp_split; ++part) {
                const uint8_t original = selected2[part];
                uint8_t winner = original;
                const float before = best_e;
                for (int index = 0; index < n2; ++index) {
                    if (index == original) continue;
                    selected2[part] = static_cast<uint8_t>(index);
                    float history[kLspCoefsMax], rec[kLspCoefsMax];
                    std::memcpy(history, saved_hist, sizeof(float) * order);
                    decode_lsp(selected1, selected2, best0, rec, history);
                    const float error = lsp_error(rec);
                    if (error < best_e) { best_e = error; winner = static_cast<uint8_t>(index); }
                }
                selected2[part] = winner;
                improved |= best_e < before;
            }
            if (!improved) break;
        }
"""
new = """        for (int refinement = 0; refinement < 2; ++refinement) {
            bool revisit_earlier = false;
            for (int part = 0; part < mtab_->lsp_split; ++part) {
                const uint8_t original = selected2[part];
                uint8_t winner = original;
                const float before = best_e;
                for (int index = 0; index < n2; ++index) {
                    if (index == original) continue;
                    selected2[part] = static_cast<uint8_t>(index);
                    float history[kLspCoefsMax], rec[kLspCoefsMax];
                    std::memcpy(history, saved_hist, sizeof(float) * order);
                    decode_lsp(selected1, selected2, best0, rec, history);
                    const float error = lsp_error(rec);
                    if (error < best_e) { best_e = error; winner = static_cast<uint8_t>(index); }
                }
                selected2[part] = winner;
                // Only a change after part 0 can invalidate an earlier part's
                // already-completed coordinate search in this sweep.
                revisit_earlier |= best_e < before && part > 0;
            }
            if (!revisit_earlier) break;
        }
"""
count = text.count(old)
if count != 1:
    raise SystemExit(f"expected one LSP refinement block, got {count}")
path.write_text(text.replace(old, new, 1))
print("applied conditional LSP refinement sweep")
