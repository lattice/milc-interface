#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <time.h>
#include <string.h>
#include <quda.h>
#include <util_quda.h>
#include <test_util.h>
#include "../tests/blas_reference.h"
#include "../tests/staggered_dslash_reference.h"
#include <gauge_field.h>
#include "external_headers/quda_milc_interface.h"

#include "include/utilities.h"





void allocateColorField(int volume, QudaPrecision prec, bool usePinnedMemory, void*& field)
{
  const int realSize = getRealSize(prec);
  int siteSize = 18;
  if(usePinnedMemory){
    cudaMallocHost((void**)&field, volume*siteSize*realSize);
  }else{
    field = (void*)malloc(volume*siteSize*realSize);
  }
  if(field == NULL){
    errorQuda("ERROR: allocateColorField failed\n");
  }
  return;
}




// No need to do this if I return a pointer
void copyGaugeField(int volume, QudaPrecision prec, void* src, void* dst)
{
  const int realSize = getRealSize(prec);
  const int siteSize = 18;
  memcpy(dst, src, 4*volume*siteSize*realSize);

  return;
}

void assignExtendedQDPGaugeField(const int dim[4], QudaPrecision precision, void* src, void** dst)
{


  const int matrix_size = 18*getRealSize(precision); 
  const int volume = getVolume(dim);

  int extended_dim[4];
  for(int dir=0; dir<4; ++dir) extended_dim[dir] = dim[dir]+4;

  const int extended_volume = getVolume(extended_dim);


  const int half_dim0 = extended_dim[0]/2;
  const int half_extended_volume = extended_volume/2;

  for(int i=0; i<extended_volume; ++i){
    int site_id = i;
	  int odd_bit = 0;

    if(i >= half_extended_volume){
      site_id -= half_extended_volume;
      odd_bit  = 1;
    }

    int za     = site_id/half_dim0;
    int x1h    = site_id - za*half_dim0;
    int zb     = za/extended_dim[1];
    int x2     = za - zb*extended_dim[1];
    int x4     = zb/extended_dim[2];
    int x3     = zb - x4*extended_dim[2];
    int x1odd  = (x2 + x3 + x4 + odd_bit) & 1;
    int x1     = 2*x1h + x1odd;

    x1 = (x1 - 2 + dim[0]) % dim[0];
    x2 = (x2 - 2 + dim[1]) % dim[1];
    x3 = (x3 - 2 + dim[2]) % dim[2];
    x4 = (x4 - 2 + dim[3]) % dim[3];

    int full_index = (x4*dim[2]*dim[1]*dim[0] + x3*dim[1]*dim[0] + x2*dim[0] + x1)>>1;
    if(odd_bit){ full_index += volume/2; }

    for(int dir=0; dir<4; ++dir){
	    char* dst_ptr = (char*)dst[dir];
	    memcpy(dst_ptr + i*matrix_size, (char*)src + (full_index*4 + dir)*matrix_size, matrix_size);
    } // end loop over directions
  } // loop over the extended volume
  return;
} // assignExtendedQDPGaugeField


void assignQDPGaugeField(const int dim[4], QudaPrecision precision, void* src, void** dst)
{


  const int matrix_size = 18*getRealSize(precision);
  const int volume = getVolume(dim);

  for(int i=0; i<volume; ++i){
    for(int dir=0; dir<4; ++dir){
	    char* dst_ptr = (char*)dst[dir];
	    memcpy(dst_ptr + i*matrix_size, (char*)src + (i*4 + dir)*matrix_size, matrix_size);
    } // end loop over directions
  } // loop over the extended volume

}



void qudaLoadFatLink(int precision, QudaFatLinkArgs_t fatlink_args, const double act_path_coeff[6], void* inlink, void* outlink)
{

  const bool usePinnedMemory = (fatlink_args.use_pinned_memory) ? true : false;

  Layout layout;
  const int* local_dim = layout.getLocalDim();
  GridInfo local_latt_info(local_dim);
  const int volume = local_latt_info.getVolume();
#ifdef MULTI_GPU  
  QudaComputeFatMethod method = QUDA_COMPUTE_FAT_STANDARD;
#else 
  QudaComputeFatMethod method = QUDA_COMPUTE_FAT_STANDARD;
#endif

  const QudaPrecision prec = (precision==1) ? QUDA_SINGLE_PRECISION : QUDA_DOUBLE_PRECISION;
#ifdef MULTI_GPU
  void* local_inlink[4];
  if(method == QUDA_COMPUTE_FAT_STANDARD){
    for(int dir=0; dir<4; ++dir){
      allocateColorField(volume, prec, usePinnedMemory, local_inlink[dir]);
    }
    assignQDPGaugeField(local_dim, prec, inlink, local_inlink);
  }else if(method == QUDA_COMPUTE_FAT_EXTENDED){	
    int extended_dim[4] = {local_dim[0]+4, local_dim[1]+4, local_dim[2]+4, local_dim[3]+4};
    for(int dir=0; dir<4; ++dir) allocateColorField(getVolume(extended_dim), prec, usePinnedMemory, local_inlink[dir]);
    assignExtendedQDPGaugeField(local_dim, prec, inlink, local_inlink);
  }
#else
  void* local_inlink = inlink;
#endif

  void* local_outlink;
  if(usePinnedMemory){
    allocateColorField(4*volume, prec, usePinnedMemory, local_outlink); 
  }else{
    local_outlink = outlink;
  }

  QudaGaugeParam param = newQudaGaugeParam();
  // Make sure all parameters are initialised, even those that aren't needed
  for(int dir=0; dir<4; ++dir) param.X[dir] = local_dim[dir];
  param.t_boundary  	   = QUDA_PERIODIC_T;
  param.anisotropy  	   = 1.0;
  param.cuda_prec_sloppy   = prec;
  param.reconstruct_sloppy = QUDA_RECONSTRUCT_NO;
  param.gauge_fix   	   = QUDA_GAUGE_FIXED_NO;
  param.ga_pad      	   = 0;
  param.packed_size 	   = 0;
  param.gaugeGiB    	   = 0;


  param.type        = QUDA_WILSON_LINKS; // an unfortunate misnomer
  param.reconstruct = QUDA_RECONSTRUCT_NO; // change this so it is read in at run time
  param.cpu_prec    = prec;
  param.cuda_prec   = prec;
#ifdef MULTI_GPU
  param.gauge_order = QUDA_QDP_GAUGE_ORDER; // In the next release, we will fix this.
#else 
  param.gauge_order = QUDA_MILC_GAUGE_ORDER;
#endif


  computeFatLinkQuda(local_outlink, (void**)local_inlink, const_cast<double*>(act_path_coeff), &param, method);

  if(usePinnedMemory){
    copyGaugeField(volume, prec, local_outlink, outlink);
    cudaFreeHost(local_outlink);
  }
  return;
}


#include <llfat_quda.h>
#include <hisq_links_quda.h>
#include <dslash_quda.h> // for initCommonConstants
#include <fat_force_quda.h> // for loadLinkToGPU and loadLinkToGPU_ex

// I need to be careful here about using pinned memory. 
// If I use pinned memory, then I need to explicitly copy the input and output data arrays. 
// Otherwise, I can just copy pointers.
void qudaLoadUnitarizedLink(int precision, QudaFatLinkArgs_t fatlink_args, const double path_coeff[6], void* inlink, void* fatlink, void* ulink)
{
  printf(" %s enters\n", __FUNCTION__);

  // Initialize unitarization parameters
  {
    const double unitarize_eps = 1e-6;
    const double max_det_error = 1e-12;
    const int reunit_allow_svd = 1;
    const int reunit_svd_only  = 0;
    const double svd_rel_error = 1e-6;
    const double svd_abs_error = 1e-6;
    hisq::setUnitarizeLinksConstants(unitarize_eps, max_det_error,
			       reunit_allow_svd, reunit_svd_only,
			       svd_rel_error, svd_abs_error);
  }

  const bool usePinnedMemory = (fatlink_args.use_pinned_memory) ? true : false;

  Layout layout;
  const int* local_dim = layout.getLocalDim();
  GridInfo local_latt_info(local_dim);
  const int volume = local_latt_info.getVolume();

  const QudaPrecision prec = (precision==1) ? QUDA_SINGLE_PRECISION : QUDA_DOUBLE_PRECISION;

  QudaComputeFatMethod method = QUDA_COMPUTE_FAT_STANDARD;

#ifdef MULTI_GPU
  void* local_inlink[4];
  if(method == QUDA_COMPUTE_FAT_STANDARD){
    for(int dir=0; dir<4; ++dir) allocateColorField(volume, prec, usePinnedMemory, local_inlink[dir]);
    assignQDPGaugeField(local_dim, prec, inlink, local_inlink);
  }else if(method == QUDA_COMPUTE_FAT_EXTENDED){
    int extended_dim[4] = {local_dim[0]+4, local_dim[1]+4, local_dim[2]+4, local_dim[3]+4};
    for(int dir=0; dir<4; ++dir) allocateColorField(getVolume(extended_dim), prec, usePinnedMemory, local_inlink[dir]);
    assignExtendedQDPGaugeField(local_dim, prec, inlink, local_inlink);
  }
#else
  void* local_inlink = inlink;
#endif

  // Allocate memory for the fatlink and unitarized link if 
  // necessary. Otherwise assign pointers.
  void* local_fatlink;
  void* local_ulink;
  if(usePinnedMemory){
    allocateColorField(4*volume, prec, usePinnedMemory, local_fatlink); 
    allocateColorField(4*volume, prec, usePinnedMemory, local_ulink); 
  }else{
    local_fatlink = fatlink;
    local_ulink   = ulink;
  }

  QudaGaugeParam param = newQudaGaugeParam();
  // Make sure all parameters are initialised, even those that aren't needed
  for(int dir=0; dir<4; ++dir) param.X[dir] = local_dim[dir];
  param.t_boundary  	   = QUDA_PERIODIC_T;
  param.anisotropy  	   = 1.0;
  param.cuda_prec_sloppy   = prec;
  param.reconstruct_sloppy = QUDA_RECONSTRUCT_NO;
  param.gauge_fix   	   = QUDA_GAUGE_FIXED_NO;
  param.ga_pad      	   = 0;
  param.packed_size 	   = 0;
  param.gaugeGiB    	   = 0;
  param.preserve_gauge     = false;


  param.type        = QUDA_WILSON_LINKS; // an unfortunate misnomer
  param.reconstruct = (fatlink_args.su3_source) ? QUDA_RECONSTRUCT_12 : QUDA_RECONSTRUCT_NO;
  param.cpu_prec    = prec;
  param.cuda_prec   = prec;
#ifdef MULTI_GPU
  param.gauge_order = QUDA_QDP_GAUGE_ORDER; // In the next release, we will fix this.
#else 
  param.gauge_order = QUDA_MILC_GAUGE_ORDER;
#endif


  // Much of this code was previously in computeFatLinkQuda
  // I have added cpuUnitarizeLink mainly for clarity. 
  // It only points to local_ulink
  static cpuGaugeField* cpuFatLink=NULL, *cpuInLink=NULL,    *cpuUnitarizedLink=NULL;
  static cudaGaugeField* cudaFatLink=NULL, *cudaInLink=NULL, *cudaUnitarizedLink=NULL;
  const int preserve_gauge = param.preserve_gauge;

  QudaGaugeParam qudaGaugeParam_ex_buf;
  QudaGaugeParam* qudaGaugeParam_ex = &qudaGaugeParam_ex_buf;
  memcpy(qudaGaugeParam_ex, &param, sizeof(QudaGaugeParam));

  for(int dir=0; dir<4; ++dir){ qudaGaugeParam_ex->X[dir] = param.X[dir]+4; }


  // fat-link padding
  setFatLinkPadding(method, &param);
  qudaGaugeParam_ex->llfat_ga_pad = param.llfat_ga_pad;
  qudaGaugeParam_ex->staple_pad   = param.staple_pad;
  qudaGaugeParam_ex->site_ga_pad  = param.site_ga_pad;
  
  GaugeFieldParam gParam(0, param);


  // create the host fatlink
  if(cpuFatLink == NULL && fatlink != NULL){
    gParam.create = QUDA_REFERENCE_FIELD_CREATE;
    gParam.link_type = QUDA_ASQTAD_FAT_LINKS;
    gParam.order = QUDA_MILC_GAUGE_ORDER;
    gParam.gauge = local_fatlink;
    cpuFatLink = new cpuGaugeField(gParam);
    if(cpuFatLink == NULL){
      errorQuda("ERROR: Creating cpuFatLink failed\n");
    }
  }else if(fatlink != NULL){
    cpuFatLink->setGauge((void**)local_fatlink);
  }

   // create the host fatlink
  if(cpuUnitarizedLink == NULL){
    gParam.create = QUDA_REFERENCE_FIELD_CREATE;
    gParam.link_type = QUDA_ASQTAD_FAT_LINKS;
    gParam.order = QUDA_MILC_GAUGE_ORDER;
    gParam.gauge = local_ulink;
    cpuUnitarizedLink = new cpuGaugeField(gParam);
    if(cpuUnitarizedLink == NULL){
      errorQuda("ERROR: Creating cpuFatLink failed\n");
    }
  }else{
    cpuUnitarizedLink->setGauge((void**)local_ulink);
  }

  // create the device fatlink 
  if(cudaFatLink == NULL){
    gParam.pad    = param.llfat_ga_pad;
    gParam.create = QUDA_ZERO_FIELD_CREATE;
    gParam.link_type = QUDA_ASQTAD_FAT_LINKS;
    gParam.order = QUDA_QDP_GAUGE_ORDER;
    gParam.reconstruct = QUDA_RECONSTRUCT_NO;
    cudaFatLink = new cudaGaugeField(gParam);
  }
  
  // create the device unitarize link - same format as the fatlink
  if(cudaUnitarizedLink == NULL){
    gParam.pad    = param.llfat_ga_pad;
    gParam.create = QUDA_ZERO_FIELD_CREATE;
    gParam.link_type = QUDA_ASQTAD_FAT_LINKS;
    gParam.order = QUDA_QDP_GAUGE_ORDER;
    gParam.reconstruct = QUDA_RECONSTRUCT_NO;
    cudaUnitarizedLink = new cudaGaugeField(gParam);
  }

  hisq::setUnitarizeLinksPadding(param.llfat_ga_pad,param.llfat_ga_pad);

  // create the host sitelink	
  if(cpuInLink == NULL){
    gParam.pad = 0; 
    gParam.create    = QUDA_REFERENCE_FIELD_CREATE;
    gParam.link_type = param.type;
    gParam.order     = param.gauge_order;
    gParam.gauge     = local_inlink;
    if(method != QUDA_COMPUTE_FAT_STANDARD){
      for(int dir=0; dir<4; ++dir) gParam.x[dir] = qudaGaugeParam_ex->X[dir];	
    }
    cpuInLink      = new cpuGaugeField(gParam);
    if(cpuInLink == NULL){
      errorQuda("ERROR: Creating cpuInLink failed\n");
    }
  }else{
    cpuInLink->setGauge((void**)local_inlink);
  }

  if(cudaInLink == NULL){
    gParam.pad         = param.site_ga_pad;
    gParam.create      = QUDA_NULL_FIELD_CREATE;
    gParam.link_type   = param.type;
    gParam.reconstruct = param.reconstruct;      
    cudaInLink = new cudaGaugeField(gParam);
  }
  
  initCommonConstants(*cudaFatLink);


  if(method == QUDA_COMPUTE_FAT_STANDARD){
    llfat_init_cuda(&param);
    param.ga_pad = param.site_ga_pad;
    if(param.gauge_order == QUDA_QDP_GAUGE_ORDER){
      loadLinkToGPU(cudaInLink, cpuInLink, &param);
    }else{
#ifdef MULTI_GPU
      errorQuda("Only QDP-ordered site links are supported in the multi-gpu standard fattening code\n");
#else
      cudaInLink->loadCPUField(*cpuInLink, QUDA_CPU_FIELD_LOCATION);
#endif
    }
  }else{
    llfat_init_cuda_ex(qudaGaugeParam_ex);
	
#ifdef MULTI_GPU
    exchange_cpu_sitelink_ex(param.X, (void**)cpuInLink->Gauge_p(), param.cpu_prec, 1);
#endif
    qudaGaugeParam_ex->ga_pad = qudaGaugeParam_ex->site_ga_pad;
    if(param.gauge_order == QUDA_QDP_GAUGE_ORDER){ 
      loadLinkToGPU_ex(cudaInLink, cpuInLink);
    }else{
      cudaInLink->loadCPUField(*cpuInLink, QUDA_CPU_FIELD_LOCATION);
    }
  } // Initialise and load siteLinks
  printf(" %s starts to compute\n", __FUNCTION__);
  // time the subroutines in computeFatLinkCore
  struct timeval time_array[4];
  // Actually do the fattening
  computeFatLinkCore(cudaInLink, const_cast<double*>(path_coeff), &param, method, cudaFatLink, time_array);
 
  printf(" %s finished compute\n", __FUNCTION__);

  int* null_dev_pointer;
  cudaMalloc((void**)&null_dev_pointer, sizeof(int));
  if(null_dev_pointer == NULL){
    errorQuda("cudaMalloc fialed for dev_pointer\n");
  }
  hisq::unitarizeLinksCuda(param, *cudaFatLink, cudaUnitarizedLink, null_dev_pointer); // unitarize on the gpu
  cudaFree(null_dev_pointer);

  // copy the fatlink back to the cpu
  if(fatlink != NULL){
#ifdef MULTI_GPU
    storeLinkToCPU(cpuFatLink, cudaFatLink, &param);
#else
    cudaFatLink->saveCPUField(*cpuFatLink, QUDA_CPU_FIELD_LOCATION);
#endif
    cudaThreadSynchronize(); checkCudaError();
    if(usePinnedMemory) copyGaugeField(volume, prec, local_fatlink, fatlink); 
  }
  // copy the unitarized link back to the cpu

#ifdef MULTI_GPU
  storeLinkToCPU(cpuUnitarizedLink, cudaUnitarizedLink, &param); 
#else
  cudaUnitarizedLink->saveCPUField(*cpuUnitarizedLink, QUDA_CPU_FIELD_LOCATION);
#endif
  cudaThreadSynchronize(); checkCudaError();
  if(usePinnedMemory) copyGaugeField(volume, prec, local_ulink, ulink);


  if (!(preserve_gauge & QUDA_FAT_PRESERVE_CPU_GAUGE) ){
    if(cpuFatLink){ delete cpuFatLink; cpuFatLink = NULL; } 
    delete cpuInLink; cpuInLink = NULL;
    delete cpuUnitarizedLink; cpuUnitarizedLink = NULL;
  }  

  if (!(preserve_gauge & QUDA_FAT_PRESERVE_GPU_GAUGE) ){
    delete cudaFatLink; cudaFatLink = NULL;
    delete cudaInLink; cudaInLink = NULL;
    delete cudaUnitarizedLink; cudaUnitarizedLink = NULL;
   }
  
  if(usePinnedMemory){
    if(fatlink != NULL) cudaFreeHost(local_fatlink);
    cudaFreeHost(local_ulink);
  }
  printf(" %s returns: reducd # of syncs\n", __FUNCTION__);
  return;
}
