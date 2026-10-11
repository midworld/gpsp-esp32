# Copyright (C) 2026 Midworld Kim
# SPDX-License-Identifier: GPL-3.0-or-later

# Generated-code extensions for host tests, e.g. make test ZB="ZBA ZBB ZBS".
# RVC=1 allows compressed forms and ZCB=1 Zcb forms. Default: none.
# Each set builds into its own directory under build/.
ZB ?=
RVC ?= 0
ZCB ?= 0
ifneq ($(filter-out ZBA ZBB ZBS,$(ZB)),)
$(error ZB accepts only ZBA, ZBB and ZBS: $(ZB))
endif
ifneq ($(filter-out 0 1,$(RVC))$(words $(RVC)),1)
$(error RVC must be 0 or 1: $(RVC))
endif
ifneq ($(filter-out 0 1,$(ZCB))$(words $(ZCB)),1)
$(error ZCB must be 0 or 1: $(ZCB))
endif
ZB_CFLAGS := $(foreach ext,$(ZB),-DGPSP_RV32_USE_$(ext)=1) \
    -DGPSP_RV32_USE_RVC=$(RVC) -DGPSP_RV32_USE_ZCB=$(ZCB)
zb_empty :=
zb_space := $(zb_empty) $(zb_empty)
OUT := $(BUILD)/$(or $(subst $(zb_space),-,$(sort $(ZB))),none)$(if $(filter 1,$(RVC)),-rvc)$(if $(filter 1,$(ZCB)),-zcb)
