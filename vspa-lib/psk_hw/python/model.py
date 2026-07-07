# SPDX-License-Identifier: BSD-3-Clause
"""Python oracle for qam modulation kernels."""

from __future__ import annotations

from pathlib import Path
import sys

import numpy as np

_COMMON = Path(__file__).resolve().parents[2] / 'common' / 'python'
if str(_COMMON) not in sys.path:
    sys.path.insert(0, str(_COMMON))

from vspa.arith import r_half_flt, r_single, r_smad

_MAP_TABLES = {
    'bpsk': np.array([-1, 1], dtype=np.float64),
    'qpsk': np.array([-1, 1], dtype=np.float64),
    '16qam': np.array([-3, 3, -1, 1], dtype=np.float64),
    '64qam': np.array([-7, 7, -1, 1, -5, 5, -3, 3], dtype=np.float64),
    '256qam': np.array([-15, 15, -1, 1, -9, 9, -7, 7, -13, 13, -3, 3, -11, 11, -5, 5], dtype=np.float64),
    '1024qam': np.array([
        -31, 31, -1, 1, -17, 17, -15, 15, -25, 25, -7, 7, -23, 23, -9, 9,
        -29, 29, -3, 3, -19, 19, -13, 13, -27, 27, -5, 5, -21, 21, -11, 11
    ], dtype=np.float64),
}


def _f32_from_u32(u):
    return np.frombuffer(np.uint32(u).tobytes(), dtype='<f4')[0]


QAM_MODES = {
    'bpsk': {'M': 1, 'norm_u32': 0x3F800000},
    'qpsk': {'M': 2, 'norm_u32': 0x3F3504F3},
    '16qam': {'M': 4, 'norm_u32': 0x3EA1E89B},
    '64qam': {'M': 6, 'norm_u32': 0x3E1E01B2},
    '256qam': {'M': 8, 'norm_u32': 0x3D9D130D},
    '1024qam': {'M': 10, 'norm_u32': 0x3D1CD80C},
}
for _m, _s in QAM_MODES.items():
    _s['norm_f32'] = _f32_from_u32(_s['norm_u32'])
    _s['map_table'] = _MAP_TABLES[_m]


def _unpack_bits_lsb_first(words_u32, n_bits):
    words = np.asarray(words_u32, dtype=np.uint32).ravel()
    needed_words = (n_bits + 31) // 32
    if words.size < needed_words:
        raise ValueError(f'need {needed_words} input words for {n_bits} bits, got {words.size}')
    bits = np.zeros(needed_words * 32, dtype=np.int8)
    for b in range(32):
        bits[b::32] = ((words >> np.uint32(b)) & np.uint32(1)).astype(np.int8)
    return bits[:n_bits]


def _pack_cfloat16(c):
    re_f16 = np.asarray(c.real, dtype=np.float16)
    im_f16 = np.asarray(c.imag, dtype=np.float16)
    re_u16 = np.frombuffer(re_f16.tobytes(), dtype='<u2').astype(np.uint32)
    im_u16 = np.frombuffer(im_f16.tobytes(), dtype='<u2').astype(np.uint32)
    return (im_u16 << np.uint32(16)) | re_u16


def r_qam_mod(bits_u32, mode):
    if mode not in QAM_MODES:
        raise ValueError(f'unknown mode {mode!r}; expected one of {sorted(QAM_MODES)}')
    spec = QAM_MODES[mode]
    m_bits = spec['M']
    map_tab = spec['map_table']
    norm = spec['norm_f32']

    words = np.asarray(bits_u32, dtype=np.uint32).ravel()
    total_bits = words.size * 32
    n_sym = total_bits // m_bits
    if n_sym == 0:
        return np.empty(0, dtype=np.uint32)

    bits = _unpack_bits_lsb_first(words, n_sym * m_bits)
    grid = bits.reshape(n_sym, m_bits).T

    if m_bits > 1:
        b_i = grid[:m_bits // 2, :]
        b_q = grid[m_bits // 2:, :]
        weights = (1 << np.arange(m_bits // 2, dtype=np.int64))
        idx_i = (weights @ b_i.astype(np.int64))
        idx_q = (weights @ b_q.astype(np.int64))
        sym_re = map_tab[idx_i]
        sym_im = map_tab[idx_q]
    else:
        idx_i = grid[0, :].astype(np.int64)
        sym_re = map_tab[idx_i]
        sym_im = np.zeros(n_sym, dtype=np.float64)

    norm_arr = np.full(n_sym, norm, dtype=np.float64)
    out_re = r_smad(sym_re, r_single(norm_arr), np.zeros(n_sym))
    out_im = r_smad(sym_im, r_single(norm_arr), np.zeros(n_sym))

    out_re_h = r_half_flt(out_re)
    out_im_h = r_half_flt(out_im)

    return _pack_cfloat16(out_re_h + 1j * out_im_h)
