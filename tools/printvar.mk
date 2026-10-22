# Used with another Makefile to print one of its variables:  make -f Makefile -f printvar.mk print-CORE
print-%:
	@echo $($*)
