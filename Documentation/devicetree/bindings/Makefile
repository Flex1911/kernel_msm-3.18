YAML_DOCS := $(shell cd $(srctree) && find Documentation/devicetree/bindings/ -name '*.yaml')

DT_DOC_CHECKER ?= echo

quiet_cmd_chk_binding = CHKDT   $@
      cmd_chk_binding = $(DT_DOC_CHECKER) "$(srctree)/$@"

%.yaml: FORCE
	$(call cmd,chk_binding)

.PHONY: checkbindings
checkbindings: $(YAML_DOCS)
