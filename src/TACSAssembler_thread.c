#include "TACSAssembler.h"
#include "tacslapack.h"

/*!
  Schedule the parts of the matrix/residual to assemble
*/
static pthread_mutex_t sched_mutex = PTHREAD_MUTEX_INITIALIZER;

void TACSAssembler::schedPthreadJob( TACSAssembler * tacs,
                                     int * index, int total_size ){
  pthread_mutex_lock(&sched_mutex);

  if (tacs->numCompletedElements < total_size){
    *index = tacs->numCompletedElements;
    tacs->numCompletedElements += 1;
  }
  else {
    *index = -1;
  }
  
  pthread_mutex_unlock(&sched_mutex);
}

/*!
  The threaded-implementation of the residual assembly 

  Note that the input must be the TACSAssemblerPthreadInfo data type.
  This function only uses the following data members:
  
  tacs:     the pointer to the TACSAssembler object
*/
void * TACSAssembler::assembleRes_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler * tacs = pinfo->tacs;

  // Allocate a temporary array large enough to store everything required  
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int dataSize = 4*s + sx;
  TacsScalar * data = new TacsScalar[ dataSize ];
  
  // Set pointers to the allocate memory
  TacsScalar *vars = &data[0];
  TacsScalar *dvars = &data[s];
  TacsScalar *ddvars = &data[2*s];
  TacsScalar *elemRes = &data[3*s];
  TacsScalar *elemXpts = &data[4*s];

  // Set the data for the auxiliary elements - if there are any
  int naux = 0, aux_count = 0;
  TACSAuxElem *aux = NULL;
  if (tacs->aux_elements){
    naux = tacs->aux_elements->getAuxElements(&aux);
  }

  while (tacs->numCompletedElements < tacs->numElements){
    int elemIndex = -1;
    TACSAssembler::schedPthreadJob(tacs, &elemIndex, tacs->numElements);
    
    if (elemIndex >= 0){
      // Get the element object
      TACSElement * element = tacs->elements[elemIndex];

      // Retrieve the variable values
      tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemIndex, 
		      tacs->Xpts, elemXpts);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localVars, vars);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localDotVars, dvars);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localDDotVars, ddvars);

      // Retrieve the number of element variables
      int nvars = element->numVariables();
      memset(elemRes, 0, nvars*sizeof(TacsScalar));
  
      // Generate the Jacobian of the element
      element->addResidual(tacs->time, elemRes, elemXpts, 
                           vars, dvars, ddvars);

      // Increment the aux counter until we possibly have
      // aux[aux_count].num == elemIndex
      while (aux_count < naux && aux[aux_count].num < elemIndex){
        aux_count++;
      }

      // Add the residual from the auxiliary elements 
      while (aux_count < naux && aux[aux_count].num == elemIndex){
        aux[aux_count].elem->addResidual(tacs->time, elemRes, elemXpts, 
                                         vars, dvars, ddvars);
        aux_count++;
      }

      // Add the values to the residual when the memory unlocks
      pthread_mutex_lock(&tacs->tacs_mutex);
      tacs->addValues(tacs->varsPerNode, elemIndex, 
		      elemRes, tacs->localRes);
      pthread_mutex_unlock(&tacs->tacs_mutex);
    }
  }

  delete [] data;

  pthread_exit(NULL);
}

/*!
  The threaded-implementation of the matrix assembly 

  Note that the input must be the TACSAssemblerPthreadInfo data type.
  This function only uses the following data members:
  
  tacs:     the pointer to the TACSAssembler object
  A:        the generic TACSMat base class
*/
void * TACSAssembler::assembleJacobian_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = 
    static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler *tacs = pinfo->tacs;
  TACSMat *A = pinfo->mat;
  double alpha = pinfo->alpha;
  double beta = pinfo->beta;
  double gamma = pinfo->gamma;
  MatrixOrientation matOr = pinfo->matOr;

  // Allocate a temporary array large enough to store everything
  // required
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int sw = tacs->maxElementIndepNodes;
  int dataSize = 4*s + sx + s*s + sw;
  TacsScalar * data = new TacsScalar[ dataSize ];
  int * idata = new int[ sw + tacs->maxElementNodes + 1 ];

  // Set pointers to the allocate memory
  TacsScalar *vars = &data[0];
  TacsScalar *dvars = &data[s];
  TacsScalar *ddvars = &data[2*s];
  TacsScalar *elemRes = &data[3*s];
  TacsScalar *elemXpts = &data[4*s];
  TacsScalar *elemWeights = &data[4*s + sx];
  TacsScalar *elemMat = &data[4*s + sx + sw];

  // Set the data for the auxiliary elements - if there are any
  int naux = 0, aux_count = 0;
  TACSAuxElem *aux = NULL;
  if (tacs->aux_elements){
    naux = tacs->aux_elements->getAuxElements(&aux);
  }

  while (tacs->numCompletedElements < tacs->numElements){
    int elemIndex = -1;
    TACSAssembler::schedPthreadJob(tacs, &elemIndex, tacs->numElements);
    
    if (elemIndex >= 0){
      // Get the element object
      TACSElement * element = tacs->elements[elemIndex];

      // Retrieve the variable values
      tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemIndex, 
		      tacs->Xpts, elemXpts);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localVars, vars);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localDotVars, dvars);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localDDotVars, ddvars);
  
      // Retrieve the number of element variables
      int nvars = element->numVariables();
      memset(elemRes, 0, nvars*sizeof(TacsScalar));
      memset(elemMat, 0, nvars*nvars*sizeof(TacsScalar));
  
      // Generate the Jacobian of the element
      element->addResidual(tacs->time, elemRes, elemXpts, 
                           vars, dvars, ddvars);
      element->addJacobian(tacs->time, elemMat, 
			   alpha, beta, gamma,
			   elemXpts, vars, dvars, ddvars);

      // Increment the aux counter until we possibly have
      // aux[aux_count].num == elemIndex
      while (aux_count < naux && aux[aux_count].num < elemIndex){
        aux_count++;
      }

      // Add the residual from the auxiliary elements 
      while (aux_count < naux && aux[aux_count].num == elemIndex){
        aux[aux_count].elem->addResidual(tacs->time, elemRes, elemXpts, 
                                         vars, dvars, ddvars);
        aux[aux_count].elem->addJacobian(tacs->time, elemMat, 
                                         alpha, beta, gamma,
                                         elemXpts, vars, dvars, ddvars);
        aux_count++;
      }
      
      pthread_mutex_lock(&tacs->tacs_mutex);
      // Add values to the residual
      tacs->addValues(tacs->varsPerNode, elemIndex, 
		      elemRes, tacs->localRes);

      // Add values to the matrix
      tacs->addMatValues(A, elemIndex, elemMat, idata, elemWeights);
      pthread_mutex_unlock(&tacs->tacs_mutex);
    }
  }

  delete [] data;
  delete [] idata;

  pthread_exit(NULL);
}

/*!
  The threaded-implementation of the matrix-type assembly 

  This function uses the following information from the
  TACSAssemblerPthreadInfo class:

  A:            the matrix to assemble (output)
  scaleFactor:  scaling factor applied to the matrix
  matType:      the matrix type defined in Element.h
  matOr:        the matrix orientation: NORMAL or TRANSPOSE
*/
void * TACSAssembler::assembleMatType_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = 
    static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler * tacs = pinfo->tacs;
  TACSMat * A = pinfo->mat;
  ElementMatrixType matType = pinfo->matType;
  MatrixOrientation matOr = pinfo->matOr;

  // Allocate a temporary array large enough to store everything required  
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int sw = tacs->maxElementIndepNodes;
  int dataSize = 3*s + sx + s*s + sw;
  TacsScalar * data = new TacsScalar[ dataSize ];
  int * idata = new int[ sw + tacs->maxElementNodes + 1 ];
  
  TacsScalar *vars = &data[0];
  TacsScalar *elemRes = &data[s];
  TacsScalar *elemXpts = &data[2*s];
  TacsScalar *elemWeights = &data[2*s + sx];
  TacsScalar *elemMat = &data[2*s + sx + sw];
  
  while (tacs->numCompletedElements < tacs->numElements){
    int elemIndex = -1;
    TACSAssembler::schedPthreadJob(tacs, &elemIndex, tacs->numElements);
    
    if (elemIndex >= 0){
      // Get the element
      TACSElement * element = tacs->elements[elemIndex];
      
      // Retrieve the variable values
      tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemIndex, 
		      tacs->Xpts, elemXpts);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localVars, vars);

      // Retrieve the type of the matrix
      element->getMatType(matType, elemMat, elemXpts, vars);
      
      pthread_mutex_lock(&tacs->tacs_mutex);
      // Add values to the matrix
      tacs->addMatValues(A, elemIndex, elemMat, idata, elemWeights);
      pthread_mutex_unlock(&tacs->tacs_mutex);
    }
  }

  delete [] data;

  pthread_exit(NULL);
}

/*
  Threaded computation of Phi^{T}*dR/dXpts 

  Here Phi is a n x numAdjoints matrix of the adjoint variables from
  several functions for the same load case.

  dR/dXpts is the derivative of the residuals w.r.t. all of the nodal
  coordinates.

  This function uses the following information from the 
  TACSAssemblerPthreadInfo class:

  numAdjoints:       the number of adjoint vectors

  This function modifies:

  adjXptSensProduct: adjXptSensProduct = Phi^{T}*dR/dXpts
*/
/*
void * TACSAssembler::adjointResXptSensProduct_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = 
    static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler * tacs = pinfo->tacs;
  TacsScalar * localAdjoint = pinfo->adjointVars;
  int numAdjoints = pinfo->numAdjoints;
  TacsScalar * adjXptSensProduct = pinfo->adjXptSensProduct;

  // The number of local variables
  int nvars = tacs->varsPerNode*tacs->numNodes;

  // Allocate a temporary array large enough to store everything required  
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int dataSize = 2*s + sx + s*sx;
  TacsScalar * data = new TacsScalar[ dataSize ];
  
  TacsScalar * elemVars = &data[0];
  TacsScalar * elemXpts = &data[s];
  TacsScalar * elemResXptSens = &data[s + sx];

  // Allocate memory for the element adjoint variables and 
  // elemXptSens = the product of the element adjoint variables and
  // the derivative of the residuals w.r.t. the nodes
  TacsScalar * elemAdjoint = new TacsScalar[ s*numAdjoints ];
  TacsScalar * elemXptSens = new TacsScalar[ sx*numAdjoints ];
    
  while (tacs->numCompletedElements < tacs->numElements){
    int elemIndex = -1;
    TACSAssembler::schedPthreadJob(tacs, &elemIndex, tacs->numElements);
    
    if (elemIndex >= 0){
      TACSElement * element = tacs->elements[elemIndex];
      
      // Get the variables and nodes for this element
      int nnodes = tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, 
				   elemIndex, tacs->Xpts, elemXpts);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localVars, elemVars);
      int nevars = tacs->varsPerNode*nnodes;
      
      // Get the adjoint variables associated with this element
      for ( int k = 0; k < numAdjoints; k++ ){
        tacs->getValues(tacs->varsPerNode, elemIndex, 
			&localAdjoint[nvars*k], &elemAdjoint[nevars*k]);
      }
      
      // Compute the derivative of the element residuals w.r.t. the
      // element nodes
      element->getResXptSens(elemResXptSens, 
                             elemVars, elemXpts);               
      
      // Increment index until the next surface traction
      while ((index < numElems) && (elemNums[index] < elemIndex)){
        index++;
      }
      
      // Add the contributions from the derivative of the consistent forces
      // w.r.t. the element nodes
      while ((index < numElems) && (elemNums[index] == elemIndex)){
        TACSElementTraction * elemTraction =
          tacs->surfaceTractions->getElement(index);
        elemTraction->addForceXptSens(lambda, elemResXptSens,
                                      elemVars, elemXpts);
        index++;
      }
      
      // Compute the product of the derivative of the residual w.r.t. the
      // nodal coordinates and the adjoint variables
      // Need to compute: 
      // elemXptSens = elemResXptSens^{T} * elemAdjoint
      int nenodes = 3*nnodes;
      TacsScalar alpha = 1.0, beta = 0.0;
      BLASgemm("T", "N", &nenodes, &numAdjoints, &nevars,
               &alpha, elemResXptSens, &nevars,
               elemAdjoint, &nevars,                 
               &beta, elemXptSens, &nenodes);
      
      // Add the values in elemXptSens into adjXptSensProduct
      pthread_mutex_lock(&tacs->tacs_mutex);
      for ( int k = 0; k < numAdjoints; k++ ){
        tacs->addValues(TACSAssembler::TACS_SPATIAL_DIM, elemIndex,
			&elemXptSens[k*nenodes],
			&adjXptSensProduct[3*tacs->numNodes*k]);
      }
      pthread_mutex_unlock(&tacs->tacs_mutex);
    }
  }
  
  delete [] data;
  delete [] elemXptSens;
  delete [] elemAdjoint;

  pthread_exit(NULL);
}
*/
/*
  Threaded computation of Phi^{T}*dR/dx

  Here Phi is a n x numAdjoints matrix of the adjoint variables from
  several functions for the same load case.

  dR/dx is the derivative of the residuals w.r.t. all of the material
  design variables.

  This function uses the following information from the 
  TACSAssemblerPthreadInfo class:

  numAdjoints:       the number of adjoint vectors
*/
void * TACSAssembler::adjointResProduct_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = 
    static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler *tacs = pinfo->tacs;
  TacsScalar *localAdjoint = pinfo->adjointVars;
  int numAdjoints = pinfo->numAdjoints;
  int numDVs = pinfo->numDesignVars;
  TacsScalar *fdvSens = new TacsScalar[ numAdjoints*numDVs ];
  memset(fdvSens, 0, numAdjoints*numDVs*sizeof(TacsScalar));

  // The number of local variables
  int nvars = tacs->varsPerNode*tacs->numNodes;

  // Allocate a temporary array large enough to store everything
  // required
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int sw = tacs->maxElementIndepNodes;
  int dataSize = 4*s + sx + s*s + sw;
  TacsScalar * data = new TacsScalar[ dataSize ];
  int * idata = new int[ sw + tacs->maxElementNodes + 1 ];

  // Set pointers to the allocate memory
  TacsScalar *vars = &data[0];
  TacsScalar *dvars = &data[s];
  TacsScalar *ddvars = &data[2*s];
  TacsScalar *elemAdjoint = &data[3*s];
  TacsScalar *elemXpts = &data[4*s];
  TacsScalar *elemWeights = &data[4*s + sx];
  TacsScalar *elemMat = &data[4*s + sx + sw];

  // Go through each element in the domain and compute the derivative
  // of the residuals with respect to each design variable and multiply by
  // the adjoint variables
  while (tacs->numCompletedElements < tacs->numElements){
    int elemIndex = -1;
    TACSAssembler::schedPthreadJob(tacs, &elemIndex, tacs->numElements);
    
    if (elemIndex >= 0){
      TACSElement * element = tacs->elements[elemIndex];

      // Retrieve the variable values
      int nnodes = 
	tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemIndex, 
			tacs->Xpts, elemXpts);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localVars, vars);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localDotVars, dvars);
      tacs->getValues(tacs->varsPerNode, elemIndex, 
		      tacs->localDDotVars, ddvars);
      int nevars = tacs->varsPerNode*nnodes;

      // Get the adjoint variables
      for ( int k = 0; k < numAdjoints; k++ ){
        tacs->getValues(tacs->varsPerNode, elemIndex,
			&localAdjoint[nvars*k], elemAdjoint);

	double scale = 1.0;
	element->addAdjResProduct(tacs->time, 
                                  scale, &fdvSens[k*numDVs], numDVs,
				  elemAdjoint, elemXpts,
				  vars, dvars, ddvars);
      }
    }
  }

  pthread_mutex_lock(&tacs->tacs_mutex);
  for ( int k = 0; k < numAdjoints*numDVs; k++ ){
    pinfo->fdvSens[k] += fdvSens[k];
  }
  pthread_mutex_unlock(&tacs->tacs_mutex);

  delete [] fdvSens;
  delete [] data;

  pthread_exit(NULL);
}

/*
  Threaded computation of a series of functions

  This function uses the following information from the threaded
  TACSAssemblerPthreadInfo class:

  iteration: the function evaluation iteration 
  numFuncs:  the number of functions
  functions: the array of functions to be evaluated
*/
void * TACSAssembler::evalFunctions_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = 
    static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler * tacs = pinfo->tacs;
  int iter = pinfo->funcIteration;
  int numFuncs = pinfo->numFuncs;
  TACSFunction ** funcs = pinfo->functions;
  TacsScalar * fXptSens = pinfo->fXptSens;

  // Determine the sizes of the work arrays
  int iwork_size = 0, work_size = 0;
  for ( int k = 0; k < numFuncs; k++ ){
    int iwsize = 0, wsize = 0;
    funcs[k]->getEvalWorkSizes(&iwsize, &wsize);
    iwork_size += iwsize;
    work_size += wsize;
  }

  // Allocate the integer data
  int * idata = new int[ 3*(numFuncs+1) + iwork_size ];
  memset(idata, 0, 3*(numFuncs+1)*sizeof(int));
  
  // Set pointers into the idata array for the work/offsets/
  int * iwork_ptr = &idata[0];
  int * work_ptr = &idata[numFuncs+1];
  int * funcElemDomainSize = &idata[2*(numFuncs+1)];
  int * iwork = &idata[3*(numFuncs+1)];

  // Determine the maximum work size amongst all functions
  for ( int k = 0; k < numFuncs; k++ ){
    int iwsize = 0, wsize = 0;
    funcs[k]->getEvalWorkSizes(&iwsize, &wsize);
    iwork_ptr[k+1] = iwork_ptr[k] + iwsize;
    work_ptr[k+1] = work_ptr[k] + wsize;
  }

  // Allocate a temporary array large enough to store
  // the temporary element data required
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int dataSize = 2*s + sx + work_size;

  TacsScalar *data = new TacsScalar[ dataSize ];  
  TacsScalar *vars = &data[0];
  TacsScalar *elemXpts = &data[s];
  TacsScalar *work = &data[s + sx];

  // Compute the total number of elements to visit for all functions
  int totalSize = 0;
  for ( int k = 0; k < numFuncs; k++ ){
    TACSFunction * function = funcs[k];

    // Determine the size of the 
    if (function->getDomain() == TACSFunction::SUB_DOMAIN){
      // Get the function sub-domain
      const int * elemSubList;
      totalSize += function->getElements(&elemSubList);
    }
    else {
      totalSize += tacs->numElements;
    }

    funcElemDomainSize[k+1] = totalSize;
  }

  // Initialize the threaded execution
  pthread_mutex_lock(&tacs->tacs_mutex);
  for ( int k = 0; k < numFuncs; k++ ){
    funcs[k]->preEvalThread(iter, &iwork[iwork_ptr[k]], 
                            &work[work_ptr[k]]);
  }
  pthread_mutex_unlock(&tacs->tacs_mutex);

  // Retrieve the function from the function list
  int funcIndex = 0;
  TACSFunction * function = funcs[0];

  // Set the work/iwork arrays to the first function
  int * iwork_array = &iwork[iwork_ptr[0]];
  TacsScalar * work_array = &work[work_ptr[0]];

  while (funcIndex < numFuncs && 
         tacs->numCompletedElements < totalSize){
    // Retrieve the next item to use
    int item = -1;
    TACSAssembler::schedPthreadJob(tacs, &item, totalSize);

    if (item >= 0){
      // Find the new function index if required
      if (item >= funcElemDomainSize[funcIndex+1]){
        while (funcIndex < numFuncs && 
               !(item >= funcElemDomainSize[funcIndex] && 
                 item <  funcElemDomainSize[funcIndex+1])){
          funcIndex++;
        }
        if (funcIndex >= numFuncs){
          break;
        }
        
        // Retrieve the new function pointer from the list
        function = funcs[funcIndex];
        
        // Set the work arrays
        iwork_array = &iwork[iwork_ptr[funcIndex]];
        work_array = &work[work_ptr[funcIndex]];
      }
      
      // Get the element index within the domain
      int elemIndex = item - funcElemDomainSize[funcIndex];
      
      if (function->getDomain() == TACSFunction::SUB_DOMAIN){
        // Get the function sub-domain
        const int * elemSubList;
        function->getElements(&elemSubList);
        
        // Retrieve the actual element number from the index list
        int elemNum = elemSubList[elemIndex];
        TACSElement * element = tacs->elements[elemNum];
        
        // Determine the values of the state variables for subElem
	tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum, 
			tacs->Xpts, elemXpts);
	tacs->getValues(tacs->varsPerNode, elemNum, 
			tacs->localVars, vars);
        
        // Call the element-wise contribution to the function
        function->elementWiseEval(iter, element, elemNum, elemXpts,
                                  vars, iwork_array, work_array);
      }
      else if (function->getDomain() == TACSFunction::ENTIRE_DOMAIN){
        int elemNum = elemIndex;
        TACSElement * element = tacs->elements[elemNum];
        
        // Determine the values of the state variables for elemNum
	tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum, 
			tacs->Xpts, elemXpts);
	tacs->getValues(tacs->varsPerNode, elemNum, 
			tacs->localVars, vars);
        
        // Call the element-wise contribution to the function
        function->elementWiseEval(iter, element, elemNum, 
                                  elemXpts, vars,
				  iwork_array, work_array);
      } 
    }
  }
    
  // Finalize the threaded execution results
  pthread_mutex_lock(&tacs->tacs_mutex);
  for ( int k = 0; k < numFuncs; k++ ){
    funcs[k]->postEvalThread(iter, &iwork[iwork_ptr[k]], 
                             &work[work_ptr[k]]);
  }
  pthread_mutex_unlock(&tacs->tacs_mutex);
  
  delete [] data;
  delete [] idata;

  pthread_exit(NULL);
}

/*
  Threaded computation of df/dXpts 

  Here f is a vector of functions of interest and Xpts are the 
  nodal locations stored in the Xpts vector internally.

  This function uses the following information from the 
  TACSAssemblerPthreadInfo class:

  numFuncs:  the number of functions
  functions: the array of functions 

  output:
  fXptSens:  the derivative of the functions w.r.t. the nodes
*/
/*
void * TACSAssembler::evalXptSens_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = 
    static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler * tacs = pinfo->tacs;
  int numFuncs = pinfo->numFuncs;
  TACSFunction ** funcs = pinfo->functions;
  TacsScalar * fXptSens = pinfo->fXptSens;

  // Determine the maximum work size amongst all functions
  int max_work_size = 0;
  for ( int k = 0; k < numFuncs; k++ ){
    int wsize = funcs[k]->getXptSensWorkSize();
    if (wsize > max_work_size){
      max_work_size = wsize;
    }
  }

  // Allocate a temporary array large enough to store
  // the temporary element data required
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int dataSize = 2*s + sx + s*sx + max_work_size;

  TacsScalar * data = new TacsScalar[ dataSize ];  
  TacsScalar * elemVars = &data[0];
  TacsScalar * elemXpts = &data[s];
  TacsScalar * elementXptSens = &data[s + sx];
  TacsScalar * work = &data[s + sx + s*sx];

  // Compute the total number of elements to visit for all functions
  int totalSize = 0;
  int * funcElemDomainSize = new int[ numFuncs+1 ];
  memset(funcElemDomainSize, 0, (numFuncs+1)*sizeof(int));

  for ( int k = 0; k < numFuncs; k++ ){
    TACSFunction * function = funcs[k];

    // Determine the size of the 
    if (function->getDomain() == TACSFunction::SUB_DOMAIN){
      // Get the function sub-domain
      const int * elemSubList;
      totalSize += function->getElements(&elemSubList);
    }
    else {
      totalSize += tacs->numElements;
    }

    funcElemDomainSize[k+1] = totalSize;
  }

  // Retrieve the function from the function list
  int funcIndex = 0;
  TACSFunction * function = funcs[0];

  while (funcIndex < numFuncs && 
         tacs->numCompletedElements < totalSize){
    // Retrieve the next item to use
    int item = -1;
    TACSAssembler::schedPthreadJob(tacs, &item, totalSize);

    if (item >= 0){
      // Find the new function index if required
      if (item >= funcElemDomainSize[funcIndex+1]){
        while (funcIndex < numFuncs && 
               !(item >= funcElemDomainSize[funcIndex] && 
                 item <  funcElemDomainSize[funcIndex+1])){
          funcIndex++;
        }
        if (funcIndex >= numFuncs){
          break;
        }

        // Retrieve the new function pointer from the list
        function = funcs[funcIndex];
      }

      // Get the element index within the domain
      int elemIndex = item - funcElemDomainSize[funcIndex];
    
      if (function->getDomain() == TACSFunction::SUB_DOMAIN){
        // Get the function sub-domain
        const int * elemSubList;
        function->getElements(&elemSubList);
      
        // Retrieve the actual element number from the index list
        int elemNum = elemSubList[elemIndex];
        TACSElement * element = tacs->elements[elemNum];

        // Determine the values of the state variables for subElem
        tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum, 
			tacs->Xpts, elemXpts);
	tacs->getValues(tacs->varsPerNode, elemNum, 
			tacs->localVars, elemVars);

        // Evaluate the element-wise sensitivity of the function
        function->elementWiseXptSens(elementXptSens, element, elemNum, 
                                     elemXpts, elemXVars, work);
      
        // Add the derivatives to the df/dXpts
        pthread_mutex_lock(&tacs->tacs_mutex);
        tacs->addValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum,
			elementXptSens, &fXptSens[3*tacs->numNodes*funcIndex]);
        pthread_mutex_unlock(&tacs->tacs_mutex);
      }
      else if (function->getDomain() == TACSFunction::ENTIRE_DOMAIN){
        int elemNum = elemIndex;
        TACSElement * element = tacs->elements[elemNum];

        // Determine the values of the state variables for elemNum
        tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum, 
			tacs->Xpts, elemXpts);
	tacs->getValues(tacs->varsPerNode, elemNum, 
			tacs->localVars, elemVars);
      
        // Evaluate the element-wise sensitivity of the function
        function->elementWiseXptSens(elementXptSens, element, elemNum, 
                                     elemXpts, elemVars, work);
      
        // Add the derivatives to the df/dXpts
        pthread_mutex_lock(&tacs->tacs_mutex);
        tacs->addValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum,
			elementXptSens, &fXptSens[3*tacs->numNodes*funcIndex]);
        pthread_mutex_unlock(&tacs->tacs_mutex);
      }
    } 
  }

  delete [] data;
  delete [] funcElemDomainSize;

  pthread_exit(NULL);
}
*/
/*
  Threaded computation of df/dx

  Here f is a vector of functions of interest and Xpts are the 
  nodal locations stored in the Xpts vector internally.

  This function uses the following information from the 
  TACSAssemblerPthreadInfo class:

  numFuncs:  the number of functions
  functions: the array of functions 

  output:
  fdvSens:  the derivative of the functions w.r.t. the nodes
*/
void * TACSAssembler::evalDVSens_thread( void * t ){
  TACSAssemblerPthreadInfo * pinfo = 
    static_cast<TACSAssemblerPthreadInfo*>(t);

  // Un-pack information for this computation
  TACSAssembler *tacs = pinfo->tacs;
  int numFuncs = pinfo->numFuncs;
  int numDVs = pinfo->numDesignVars;
  TACSFunction **funcs = pinfo->functions;

  // Determine the maximum work size amongst all functions
  int max_work_size = 0;
  for ( int k = 0; k < numFuncs; k++ ){
    int wsize = funcs[k]->getDVSensWorkSize();
    if (wsize > max_work_size){
      max_work_size = wsize;
    }
  }

  // Allocate space for the local contributions to the dvSens
  TacsScalar * fdvSens = new TacsScalar[ numFuncs*numDVs ];
  memset(fdvSens, 0, numFuncs*numDVs*sizeof(TacsScalar));

  // Allocate a temporary array large enough to store
  // the temporary element data required
  int s = tacs->maxElementSize;
  int sx = 3*tacs->maxElementNodes;
  int dataSize = 2*s + sx + max_work_size;

  TacsScalar * data = new TacsScalar[ dataSize ];  
  TacsScalar * elemVars = &data[0];
  TacsScalar * elemXpts = &data[s];
  TacsScalar * work = &data[s + sx];

  // Compute the total number of elements to visit for all functions
  int totalSize = 0;
  int * funcElemDomainSize = new int[ numFuncs+1 ];
  memset(funcElemDomainSize, 0, (numFuncs+1)*sizeof(int));

  for ( int k = 0; k < numFuncs; k++ ){
    TACSFunction * function = funcs[k];

    // Determine the size of the 
    if (function->getDomain() == TACSFunction::SUB_DOMAIN){
      // Get the function sub-domain
      const int * elemSubList;
      totalSize += function->getElements(&elemSubList);
    }
    else {
      totalSize += tacs->numElements;
    }

    funcElemDomainSize[k+1] = totalSize;
  }
  
  // Retrieve the function from the function list
  int funcIndex = 0;
  TACSFunction * function = funcs[0];

  while (funcIndex < numFuncs && 
         tacs->numCompletedElements < totalSize){
    // Retrieve the next item to use
    int item = -1;
    TACSAssembler::schedPthreadJob(tacs, &item, totalSize);

    if (item >= 0){
      // Find the new function index if required
      if (item >= funcElemDomainSize[funcIndex+1]){
        while (funcIndex < numFuncs && 
               !(item >= funcElemDomainSize[funcIndex] && 
                 item <  funcElemDomainSize[funcIndex+1])){
          funcIndex++;
        }
        if (funcIndex >= numFuncs){
          break;
        }

        // Retrieve the new function pointer from the list
        function = funcs[funcIndex];
      }

      // Get the element index within the domain
      int elemIndex = item - funcElemDomainSize[funcIndex];

      if (function->getDomain() == TACSFunction::SUB_DOMAIN){
        // Get the function sub-domain
        const int * elemSubList;
        function->getElements(&elemSubList);
      
        // Retrieve the actual element number from the index list
        int elemNum = elemSubList[elemIndex];
        TACSElement * element = tacs->elements[elemNum];

        // Determine the values of the state variables for subElem
        tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum, 
			tacs->Xpts, elemXpts);
	tacs->getValues(tacs->varsPerNode, elemNum, 
			tacs->localVars, elemVars);

        // Evaluate the element-wise sensitivity of the function
        function->elementWiseDVSens(&fdvSens[funcIndex*numDVs], numDVs,
                                    element, elemNum, 
                                    elemXpts, elemVars, work);	      
      }
      else if (function->getDomain() == TACSFunction::ENTIRE_DOMAIN){
        int elemNum = elemIndex;
        TACSElement * element = tacs->elements[elemNum];
      
        // Determine the values of the state variables for elemNum
        tacs->getValues(TACSAssembler::TACS_SPATIAL_DIM, elemNum, 
			tacs->Xpts, elemXpts);
	tacs->getValues(tacs->varsPerNode, elemNum, 
			tacs->localVars, elemVars);
      
        // Evaluate the element-wise sensitivity of the function
        function->elementWiseDVSens(&fdvSens[funcIndex*numDVs], numDVs,
                                    element, elemNum, 
                                    elemXpts, elemVars, work);
      }
    }
  } 

  pthread_mutex_lock(&tacs->tacs_mutex);
  for ( int k = 0; k < numFuncs*numDVs; k++ ){
    pinfo->fdvSens[k] += fdvSens[k];
  }
  pthread_mutex_unlock(&tacs->tacs_mutex);

  delete [] data;
  delete [] funcElemDomainSize;
  delete [] fdvSens;

  pthread_exit(NULL);
}

