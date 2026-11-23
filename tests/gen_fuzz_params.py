# Omni Sampler 2.4.32: tests/fuzz_params.inc for test_synth_fuzz, from params.json: the SYNTH, FX and FX BANK controls
# (names, readouts, the keypad, folder steps and LOAD left out).   python3 -I tests/gen_fuzz_params.py   (from omni-1.8.12)
import json, re
p = json.load(open('params.json'))['params']
inc = re.compile(r'^(syn_|sa_|sb_|eq_|comp|cho_|dly_|phs_|flg_|trm_|lofi_|rev_|drive|limiter|fxb_|cutoff$|resonance$|filter_|lfo[12]_|mod[1-8]_|volume$)')
exc = re.compile(r'(_name$|_info$|_w\d+$|^syn_on$|_folder$|_fprev$|^fxb_(pad|num|num_tap|k\d|kclr|kenter|info)$|_load$|gr_meter)')
out = []
for x in p:
    k = x['key']
    if not inc.search(k) or exc.search(k): continue
    if 'options' in x: lo, hi, mom = 0, len(x['options']) - 1, 0
    elif 'min' in x: lo, hi, mom = x['min'], x['max'], 1 if x.get('momentary') else 0
    else: continue
    out.append('{"%s",%g,%g,%d},' % (k, lo, hi, mom))
open('tests/fuzz_params.inc', 'w').write('\n'.join(out) + '\n')
print(len(out), 'params')
