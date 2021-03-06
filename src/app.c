/*
 * File: app.c
 * Project: src
 * Created Date: 22/04/2022
 * Author: Shun Suzuki
 * -----
 * Last Modified: 10/06/2022
 * Modified By: Shun Suzuki (suzuki@hapis.k.u-tokyo.ac.jp)
 * -----
 * Copyright (c) 2022 Shun Suzuki. All rights reserved.
 *
 */

#include "app.h"

#include "iodefine.h"
#include "params.h"
#include "utils.h"

#define CPU_VERSION (0x82) /* v2.2 */

#define MOD_BUF_SEGMENT_SIZE_WIDTH (15)
#define MOD_BUF_SEGMENT_SIZE (1 << MOD_BUF_SEGMENT_SIZE_WIDTH)
#define MOD_BUF_SEGMENT_SIZE_MASK (MOD_BUF_SEGMENT_SIZE - 1)

#define POINT_STM_BUF_SEGMENT_SIZE_WIDTH (11)
#define POINT_STM_BUF_SEGMENT_SIZE (1 << POINT_STM_BUF_SEGMENT_SIZE_WIDTH)
#define POINT_STM_BUF_SEGMENT_SIZE_MASK (POINT_STM_BUF_SEGMENT_SIZE - 1)

#define GAIN_STM_BUF_SEGMENT_SIZE_WIDTH (5)
#define GAIN_STM_BUF_SEGMENT_SIZE (1 << GAIN_STM_BUF_SEGMENT_SIZE_WIDTH)
#define GAIN_STM_BUF_SEGMENT_SIZE_MASK (GAIN_STM_BUF_SEGMENT_SIZE - 1)

#define GAIN_DATA_MODE_PHASE_DUTY_FULL (0x0001)
#define GAIN_DATA_MODE_PHASE_FULL (0x0002)
#define GAIN_DATA_MODE_PHASE_HALF (0x0004)

#define MSG_CLEAR (0x00)
#define MSG_RD_CPU_VERSION (0x01)
#define MSG_RD_FPGA_VERSION (0x03)
#define MSG_RD_FPGA_FUNCTION (0x04)
#define MSG_BEGIN (0x05)
#define MSG_END (0xF0)

extern RX_STR0 _sRx0;
extern RX_STR1 _sRx1;
extern TX_STR _sTx;

// fire when ethercat packet arrives
extern void recv_ethercat(void);
// fire once after power on
extern void init_app(void);
// fire periodically with 1ms interval
extern void update(void);

typedef enum {
  LEGACY_MODE = 1 << CTL_REG_LEGACY_MODE_BIT,
  FORCE_FAN = 1 << CTL_REG_FORCE_FAN_BIT,
  OP_MODE = 1 << CTL_REG_OP_MODE_BIT,
  STM_GAIN_MODE = 1 << CTL_REG_STM_GAIN_MODE_BIT,
  READS_FPGA_INFO = 1 << CTL_REG_READS_FPGA_INFO_BIT,
  SYNC = 1 << CTL_REG_SYNC_BIT,
} FPGAControlFlags;

typedef enum {
  MOD = 1 << 0,
  MOD_BEGIN = 1 << 1,
  MOD_END = 1 << 2,
  CONFIG_EN_N = 1 << 0,
  CONFIG_SILENCER = 1 << 1,
  CONFIG_SYNC = 1 << 2,
  WRITE_BODY = 1 << 3,
  STM_BEGIN = 1 << 4,
  STM_END = 1 << 5,
  IS_DUTY = 1 << 6,
  MOD_DELAY = 1 << 7
} CPUControlFlags;

typedef struct {
  uint8_t msg_id;
  uint8_t fpga_ctl_reg;
  uint8_t cpu_ctl_reg;
  uint8_t size;
  union {
    struct {
      uint32_t freq_div;
      uint8_t data[120];
    } MOD_HEAD;
    struct {
      uint8_t data[124];
    } MOD_BODY;
    struct {
      uint16_t cycle;
      uint16_t step;
      uint8_t _data[120];
    } SILENT;
  } DATA;
} GlobalHeader;

typedef struct {
  union {
    struct {
      uint16_t data[TRANS_NUM];
    } NORMAL;
    struct {
      uint16_t cycle[TRANS_NUM];
    } CYCLE;
    struct {
      uint16_t data[TRANS_NUM];
    } POINT_STM_HEAD;
    struct {
      uint16_t data[TRANS_NUM];
    } POINT_STM_BODY;
    struct {
      uint16_t data[TRANS_NUM];
    } GAIN_STM_HEAD;
    struct {
      uint16_t data[TRANS_NUM];
    } GAIN_STM_BODY;
    struct {
      uint16_t data[TRANS_NUM];
    } MOD_DELAY_DATA;
  } DATA;
} Body;

static volatile uint16_t _ack = 0;
static volatile uint8_t _msg_id = 0;
static volatile bool_t _read_fpga_info;

static volatile uint16_t _cycle[TRANS_NUM];

static volatile uint32_t _mod_cycle = 0;

static volatile uint32_t _stm_cycle = 0;
static volatile uint16_t _seq_gain_data_mode = GAIN_DATA_MODE_PHASE_DUTY_FULL;

#define BUF_SIZE (32)
static volatile GlobalHeader _head_buf[BUF_SIZE];
static volatile Body _body_buf[BUF_SIZE];
volatile uint32_t _write_cursor;
volatile uint32_t _read_cursor;

static volatile GlobalHeader _head;
static volatile Body _body;

bool_t push(const volatile GlobalHeader* head, const volatile Body* body) {
  uint32_t next;
  next = _write_cursor + 1;

  if (next >= BUF_SIZE) next = 0;

  if (next == _read_cursor) return false;

  memcpy_volatile(&_head_buf[_write_cursor], head, sizeof(GlobalHeader));
  memcpy_volatile(&_body_buf[_write_cursor], body, sizeof(Body));

  // dmb?

  _write_cursor = next;

  return true;
}

bool_t pop(volatile GlobalHeader* head, volatile Body* body) {
  uint32_t next;

  if (_read_cursor == _write_cursor) return false;

  // dmb?

  memcpy_volatile(head, &_head_buf[_read_cursor], sizeof(GlobalHeader));
  memcpy_volatile(body, &_body_buf[_read_cursor], sizeof(Body));

  next = _read_cursor + 1;
  if (next >= BUF_SIZE) next = 0;

  _read_cursor = next;

  return true;
}

void synchronize(const volatile GlobalHeader* header, const volatile Body* body) {
  const volatile uint16_t* cycle = body->DATA.CYCLE.cycle;
  volatile uint64_t next_sync0 = ECATC.DC_CYC_START_TIME.LONGLONG;

  bram_cpy_volatile(BRAM_SELECT_CONTROLLER, BRAM_ADDR_CYCLE_BASE, cycle, TRANS_NUM);
  bram_cpy_volatile(BRAM_SELECT_CONTROLLER, BRAM_ADDR_EC_SYNC_TIME_0, (volatile uint16_t*)&next_sync0, sizeof(uint64_t) >> 1);

  bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_CTL_REG, header->fpga_ctl_reg | SYNC);

  memcpy_volatile(_cycle, cycle, TRANS_NUM * sizeof(uint16_t));
}

void write_mod(const volatile GlobalHeader* header) {
  uint32_t freq_div;
  uint16_t* data;
  uint32_t segment_capacity;
  uint32_t write = header->size;

  if ((header->cpu_ctl_reg & MOD_BEGIN) != 0) {
    _mod_cycle = 0;
    bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_MOD_ADDR_OFFSET, 0);
    freq_div = header->DATA.MOD_HEAD.freq_div;
    bram_cpy(BRAM_SELECT_CONTROLLER, BRAM_ADDR_MOD_FREQ_DIV_0, (uint16_t*)&freq_div, sizeof(uint32_t) >> 1);
    data = (uint16_t*)header->DATA.MOD_HEAD.data;
  } else {
    data = (uint16_t*)header->DATA.MOD_BODY.data;
  }

  segment_capacity = (_mod_cycle & ~MOD_BUF_SEGMENT_SIZE_MASK) + MOD_BUF_SEGMENT_SIZE - _mod_cycle;
  if (write <= segment_capacity) {
    bram_cpy(BRAM_SELECT_MOD, (_mod_cycle & MOD_BUF_SEGMENT_SIZE_MASK) >> 1, data, (write + 1) >> 1);
    _mod_cycle += write;
  } else {
    bram_cpy(BRAM_SELECT_MOD, (_mod_cycle & MOD_BUF_SEGMENT_SIZE_MASK) >> 1, data, segment_capacity >> 1);
    _mod_cycle += segment_capacity;
    data += segment_capacity;
    bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_MOD_ADDR_OFFSET, (_mod_cycle & ~MOD_BUF_SEGMENT_SIZE_MASK) >> MOD_BUF_SEGMENT_SIZE_WIDTH);
    bram_cpy(BRAM_SELECT_MOD, (_mod_cycle & MOD_BUF_SEGMENT_SIZE_MASK) >> 1, data, (write - segment_capacity + 1) >> 1);
    _mod_cycle += write - segment_capacity;
  }

  if ((header->cpu_ctl_reg & MOD_END) != 0) bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_MOD_CYCLE, max(1, _mod_cycle) - 1);
}

void config_silencer(const volatile GlobalHeader* header) {
  uint16_t step = header->DATA.SILENT.step;
  uint16_t cycle = header->DATA.SILENT.cycle;
  bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_SILENT_STEP, step);
  bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_SILENT_CYCLE, cycle);
}

static void set_mod_delay(const volatile Body* body) {
  bram_cpy_volatile(BRAM_SELECT_CONTROLLER, BRAM_ADDR_MOD_DELAY_BASE, body->DATA.MOD_DELAY_DATA.data, TRANS_NUM);
}

static void write_normal_op_legacy(const volatile Body* body) {
  volatile uint16_t* base = (volatile uint16_t*)FPGA_BASE;
  uint16_t addr = get_addr(BRAM_SELECT_NORMAL, 0);
  uint32_t cnt = TRANS_NUM;
  volatile uint16_t* dst = &base[addr];
  const volatile uint16_t* src = body->DATA.NORMAL.data;
  while (cnt--) {
    *dst = *src++;
    dst += 2;
  }
}

static void write_normal_op_raw(const volatile Body* body, bool_t is_duty) {
  volatile uint16_t* base = (volatile uint16_t*)FPGA_BASE;
  uint16_t addr = get_addr(BRAM_SELECT_NORMAL, 0);
  uint32_t cnt = TRANS_NUM;
  volatile uint16_t* dst = &base[addr] + (is_duty ? 1 : 0);
  const volatile uint16_t* src = body->DATA.NORMAL.data;
  while (cnt-- > 0) {
    *dst = *src++;
    dst += 2;
  }
}

static void write_normal_op(const volatile GlobalHeader* header, const volatile Body* body) {
  if (header->fpga_ctl_reg & LEGACY_MODE) {
    write_normal_op_legacy(body);
  } else {
    write_normal_op_raw(body, (header->cpu_ctl_reg & IS_DUTY) != 0);
  }
}

static void write_point_stm(const volatile GlobalHeader* header, const volatile Body* body) {
  volatile uint16_t* base = (volatile uint16_t*)FPGA_BASE;
  uint16_t addr;
  volatile uint16_t* dst;
  const volatile uint16_t* src;
  uint32_t freq_div;
  uint32_t sound_speed;
  uint32_t size, cnt;
  uint32_t segment_capacity;

  if ((header->cpu_ctl_reg & STM_BEGIN) != 0) {
    _stm_cycle = 0;
    bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_ADDR_OFFSET, 0);

    size = body->DATA.POINT_STM_HEAD.data[0];
    freq_div = (body->DATA.POINT_STM_HEAD.data[2] << 16) | body->DATA.POINT_STM_HEAD.data[1];
    sound_speed = (body->DATA.POINT_STM_HEAD.data[4] << 16) | body->DATA.POINT_STM_HEAD.data[3];

    bram_cpy(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_FREQ_DIV_0, (uint16_t*)&freq_div, sizeof(uint32_t) >> 1);
    bram_cpy(BRAM_SELECT_CONTROLLER, BRAM_ADDR_SOUND_SPEED_0, (uint16_t*)&sound_speed, sizeof(uint32_t) >> 1);
    src = body->DATA.POINT_STM_HEAD.data + 5;
  } else {
    size = body->DATA.POINT_STM_BODY.data[0];
    src = body->DATA.POINT_STM_BODY.data + 1;
  }

  segment_capacity = (_stm_cycle & ~POINT_STM_BUF_SEGMENT_SIZE_MASK) + POINT_STM_BUF_SEGMENT_SIZE - _stm_cycle;
  if (size <= segment_capacity) {
    cnt = size;
    addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & POINT_STM_BUF_SEGMENT_SIZE_MASK) << 3);
    dst = &base[addr];
    while (cnt--) {
      *dst++ = *src++;
      *dst++ = *src++;
      *dst++ = *src++;
      *dst++ = *src++;
      dst += 4;
    }
    _stm_cycle += size;
  } else {
    cnt = segment_capacity;
    addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & POINT_STM_BUF_SEGMENT_SIZE_MASK) << 3);
    dst = &base[addr];
    while (cnt--) {
      *dst++ = *src++;
      *dst++ = *src++;
      *dst++ = *src++;
      *dst++ = *src++;
      dst += 4;
    }
    _stm_cycle += segment_capacity;

    bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_ADDR_OFFSET,
               (_stm_cycle & ~POINT_STM_BUF_SEGMENT_SIZE_MASK) >> POINT_STM_BUF_SEGMENT_SIZE_WIDTH);

    cnt = size - segment_capacity;
    addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & POINT_STM_BUF_SEGMENT_SIZE_MASK) << 3);
    dst = &base[addr];
    while (cnt--) {
      *dst++ = *src++;
      *dst++ = *src++;
      *dst++ = *src++;
      *dst++ = *src++;
      dst += 4;
    }
    _stm_cycle += size - segment_capacity;
  }

  if ((header->cpu_ctl_reg & STM_END) != 0) bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_CYCLE, max(1, _stm_cycle) - 1);
}

static void write_gain_stm(const volatile GlobalHeader* header, const volatile Body* body) {
  volatile uint16_t* base = (volatile uint16_t*)FPGA_BASE;
  uint16_t addr;
  volatile uint16_t* dst;
  const volatile uint16_t* src;
  uint32_t freq_div;
  uint32_t cnt;
  uint16_t phase;

  if ((header->cpu_ctl_reg & STM_BEGIN) != 0) {
    _stm_cycle = 0;
    bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_ADDR_OFFSET, 0);
    freq_div = (body->DATA.GAIN_STM_HEAD.data[1] << 16) | body->DATA.GAIN_STM_HEAD.data[0];
    bram_cpy(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_FREQ_DIV_0, (uint16_t*)&freq_div, sizeof(uint32_t) >> 1);
    _seq_gain_data_mode = body->DATA.GAIN_STM_HEAD.data[2];
    return;
  }

  src = body->DATA.GAIN_STM_BODY.data;

  addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & GAIN_STM_BUF_SEGMENT_SIZE_MASK) << 9);

  switch (_seq_gain_data_mode) {
    case GAIN_DATA_MODE_PHASE_DUTY_FULL:
      if ((header->fpga_ctl_reg & LEGACY_MODE) != 0) {
        dst = &base[addr];
        _stm_cycle += 1;
      } else {
        if ((header->cpu_ctl_reg & IS_DUTY) != 0) {
          dst = &base[addr] + 1;
          _stm_cycle += 1;
        } else {
          dst = &base[addr];
        }
      }
      cnt = TRANS_NUM;
      while (cnt--) {
        *dst = *src++;
        dst += 2;
      }
      break;
    case GAIN_DATA_MODE_PHASE_FULL:
      if ((header->fpga_ctl_reg & LEGACY_MODE) != 0) {
        dst = &base[addr];
        cnt = TRANS_NUM;
        while (cnt--) {
          *dst = 0xFF00 | ((*src++) & 0x00FF);
          dst += 2;
        }
        _stm_cycle += 1;
        src = body->DATA.GAIN_STM_BODY.data;
        addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & GAIN_STM_BUF_SEGMENT_SIZE_MASK) << 9);
        dst = &base[addr];
        cnt = TRANS_NUM;
        while (cnt--) {
          *dst = 0xFF00 | (((*src++) >> 8) & 0x00FF);
          dst += 2;
        }
        _stm_cycle += 1;
      } else {
        if ((header->cpu_ctl_reg & IS_DUTY) != 0) break;
        dst = &base[addr];
        cnt = 0;
        while (cnt++ < TRANS_NUM) {
          *dst++ = *src++;
          *dst++ = _cycle[cnt] >> 1;
        }
        _stm_cycle += 1;
      }
      break;
    case GAIN_DATA_MODE_PHASE_HALF:
      if ((header->fpga_ctl_reg & LEGACY_MODE) == 0) break;
      dst = &base[addr];
      cnt = TRANS_NUM;
      while (cnt--) {
        phase = (*src++) & 0x000F;
        *dst = 0xFF00 | (phase << 4) | phase;
        dst += 2;
      }
      _stm_cycle += 1;

      src = body->DATA.GAIN_STM_BODY.data;
      addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & GAIN_STM_BUF_SEGMENT_SIZE_MASK) << 9);
      dst = &base[addr];
      cnt = TRANS_NUM;
      while (cnt--) {
        phase = ((*src++) >> 4) & 0x000F;
        *dst = 0xFF00 | (phase << 4) | phase;
        dst += 2;
      }
      _stm_cycle += 1;

      src = body->DATA.GAIN_STM_BODY.data;
      addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & GAIN_STM_BUF_SEGMENT_SIZE_MASK) << 9);
      dst = &base[addr];
      cnt = TRANS_NUM;
      while (cnt--) {
        phase = ((*src++) >> 8) & 0x000F;
        *dst = 0xFF00 | (phase << 4) | phase;
        dst += 2;
      }
      _stm_cycle += 1;

      src = body->DATA.GAIN_STM_BODY.data;
      addr = get_addr(BRAM_SELECT_STM, (_stm_cycle & GAIN_STM_BUF_SEGMENT_SIZE_MASK) << 9);
      dst = &base[addr];
      cnt = TRANS_NUM;
      while (cnt--) {
        phase = ((*src++) >> 12) & 0x000F;
        *dst = 0xFF00 | (phase << 4) | phase;
        dst += 2;
      }
      _stm_cycle += 1;
      break;
    default:
      if ((header->fpga_ctl_reg & LEGACY_MODE) != 0) {
        dst = &base[addr];
        _stm_cycle += 1;
      } else {
        if ((header->cpu_ctl_reg & IS_DUTY) != 0) {
          dst = &base[addr] + 1;
          _stm_cycle += 1;
        } else {
          dst = &base[addr];
        }
      }
      cnt = TRANS_NUM;
      while (cnt--) {
        *dst = *src++;
        dst += 2;
      }
      break;
  }

  if ((_stm_cycle & GAIN_STM_BUF_SEGMENT_SIZE_MASK) == 0)
    bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_ADDR_OFFSET, (_stm_cycle & ~GAIN_STM_BUF_SEGMENT_SIZE_MASK) >> GAIN_STM_BUF_SEGMENT_SIZE_WIDTH);

  if ((header->cpu_ctl_reg & STM_END) != 0) bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_STM_CYCLE, max(1, _stm_cycle) - 1);
}

static void clear(void) {
  uint32_t freq_div_4k = 40960;

  _read_fpga_info = false;
  bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_CTL_REG, LEGACY_MODE);

  bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_SILENT_STEP, 10);
  bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_SILENT_CYCLE, 4096);

  _stm_cycle = 0;

  _mod_cycle = 2;
  bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_MOD_CYCLE, max(1, _mod_cycle) - 1);
  bram_cpy(BRAM_SELECT_CONTROLLER, BRAM_ADDR_MOD_FREQ_DIV_0, (uint16_t*)&freq_div_4k, sizeof(uint32_t) >> 1);
  bram_write(BRAM_SELECT_MOD, 0, 0x0000);

  bram_set(BRAM_SELECT_NORMAL, 0, 0x0000, TRANS_NUM << 1);

  memset_volatile(&_head_buf[0], 0x00, sizeof(GlobalHeader) * BUF_SIZE);
  memset_volatile(&_body_buf[0], 0x00, sizeof(Body) * BUF_SIZE);
  memset_volatile(&_head, 0x00, sizeof(GlobalHeader));
  memset_volatile(&_body, 0x00, sizeof(Body));
}

inline static uint16_t get_cpu_version(void) { return CPU_VERSION; }
inline static uint16_t get_fpga_version(void) { return bram_read(BRAM_SELECT_CONTROLLER, BRAM_ADDR_VERSION_NUM); }
inline static uint16_t read_fpga_info(void) { return bram_read(BRAM_SELECT_CONTROLLER, BRAM_ADDR_FPGA_INFO); }

void init_app(void) { clear(); }

void process() {
  uint16_t ctl_reg;
  if (pop(&_head, &_body)) {
    ctl_reg = _head.fpga_ctl_reg;
    bram_write(BRAM_SELECT_CONTROLLER, BRAM_ADDR_CTL_REG, ctl_reg);

    if ((_head.cpu_ctl_reg & MOD) != 0)
      write_mod(&_head);
    else if ((_head.cpu_ctl_reg & CONFIG_SILENCER) != 0) {
      config_silencer(&_head);
    };

    if ((_head.cpu_ctl_reg & WRITE_BODY) == 0) return;

    if ((_head.cpu_ctl_reg & MOD_DELAY) != 0) {
      set_mod_delay(&_body);
      return;
    }

    if ((ctl_reg & OP_MODE) == 0) {
      write_normal_op(&_head, &_body);
      return;
    }

    if ((ctl_reg & STM_GAIN_MODE) == 0)
      write_point_stm(&_head, &_body);
    else
      write_gain_stm(&_head, &_body);
  }
}

void update(void) {
  process();

  switch (_msg_id) {
    case MSG_RD_CPU_VERSION:
    case MSG_RD_FPGA_VERSION:
    case MSG_RD_FPGA_FUNCTION:
      break;
    default:
      if (_read_fpga_info) _ack = (_ack & 0xFF00) | read_fpga_info();
      break;
  }
  _sTx.ack = _ack;
}

void recv_ethercat(void) {
  GlobalHeader* header = (GlobalHeader*)(_sRx1.data);
  Body* body = (Body*)(_sRx0.data);
  if (header->msg_id == _msg_id) return;
  _msg_id = header->msg_id;
  _ack = ((uint16_t)(header->msg_id)) << 8;
  _read_fpga_info = (header->fpga_ctl_reg & READS_FPGA_INFO) != 0;
  if (_read_fpga_info) _ack = (_ack & 0xFF00) | read_fpga_info();

  switch (_msg_id) {
    case MSG_CLEAR:
      clear();
      break;
    case MSG_RD_CPU_VERSION:
      _ack = (_ack & 0xFF00) | (get_cpu_version() & 0xFF);
      break;
    case MSG_RD_FPGA_VERSION:
      _ack = (_ack & 0xFF00) | (get_fpga_version() & 0xFF);
      break;
    case MSG_RD_FPGA_FUNCTION:
      _ack = (_ack & 0xFF00) | ((get_fpga_version() >> 8) & 0xFF);
      break;
    default:
      if (_msg_id > MSG_END) break;

      if (((header->cpu_ctl_reg & MOD) == 0) && ((header->cpu_ctl_reg & CONFIG_SYNC) != 0)) {
        synchronize(header, body);
        break;
      }

      while (!push(header, body)) {
      }

      break;
  }
  _sTx.ack = _ack;
}
