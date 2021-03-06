SUBDIRS = base crypto fs services threads .
DIST_SUBDIRS = $(SUBDIRS) tests

if BUILD_TESTS
SUBDIRS += tests
endif

service_libs =

if WITH_AWS
service_libs += services/aws/libs3fuse_services_aws.a
endif

if WITH_FVS
service_libs += services/fvs/libs3fuse_services_fvs.a
endif

if WITH_GS
service_libs += services/gs/libs3fuse_services_gs.a
endif

# the order of archives in s3fuse_libs matters -- they must be listed such that if A
# requires symbols provided by B, then A must appear before B

s3fuse_libs = \
	fs/libs3fuse_fs.a \
	$(service_libs) \
	threads/libs3fuse_threads.a \
	services/libs3fuse_services.a \
	crypto/libs3fuse_crypto.a \
	base/libs3fuse_base.a

init_src = \
	init.cc \
	init.h

bin_PROGRAMS = s3fuse s3fuse_sha256_sum s3fuse_vol_key

if WITH_GS
bin_PROGRAMS += s3fuse_gs_get_token
endif

s3fuse_SOURCES = $(init_src) main.cc operations.cc operations.h
s3fuse_LDADD = $(s3fuse_libs) $(LDADD)

s3fuse_gs_get_token_SOURCES = gs_get_token.cc
s3fuse_gs_get_token_LDADD = $(s3fuse_libs) $(LDADD)

s3fuse_sha256_sum_SOURCES = sha256_sum.cc
s3fuse_sha256_sum_LDADD = $(s3fuse_libs) $(LDADD)

s3fuse_vol_key_SOURCES = $(init_src) vol_key.cc
s3fuse_vol_key_LDADD = $(s3fuse_libs) $(LDADD)
