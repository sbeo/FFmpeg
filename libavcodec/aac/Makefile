clean::
		$(RM) $(CLEANSUFFIXES:%=libavcodec/aac/%)

OBJS-$(CONFIG_AAC_DECODER)          +=  aac/aacdec.o aac/aacdec_tab.o \
                                        aac/aacdec_float.o aac/aacdec_usac.o \
                                        aac/aacdec_ac.o aac/aacdec_lpd.o \
                                        aac/aacdec_usac_mps212.o \
                                        aac/aacdec_bsac.o aac/aacdec_bsac_tab.o
OBJS-$(CONFIG_AAC_FIXED_DECODER)    +=  aac/aacdec.o aac/aacdec_tab.o \
                                        aac/aacdec_fixed.o
