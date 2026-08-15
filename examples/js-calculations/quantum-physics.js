// (c) 2020-2022 Manuel Joffre
/*******************************************************************************
 * 
 * www.quantum-physics.polytechnique.fr
 * 
 * Javascript library for quantum physics simulations
 *
 ******************************************************************************/


/*******************************************************************************
 * 
 * Mathematical functions useful in quantum physics
 * 
 * Hermite functions, spherical harmonics
 * 
 ******************************************************************************/

function hermite(x, n) {
    var factor = Math.sqrt(Math.PI);
    var result;
    if (n>1) {
        var penultimate;
        var previous = 1;
        factor = factor*2;
        result = 2*x;
        for (var j=2; j<=n; j++) {
            penultimate = previous;
            previous = result;
            result = 2*x*previous-2*(j-1)*penultimate;
            factor = factor*2*j;
        }
    }
    else if (n===1) {
        result = 2*x;
        factor = factor*2;
    }
    else // n=0
        result = 1;
    return result*Math.exp(-x*x/2)/Math.sqrt(factor);
}

// Maximum value of principal quantum number when calculating atomic orbitals
const nMax = 51;

// Maximum value of angular momentum when calculating spherical harmonics
const ellMax = nMax-1;

var nTable = 1001;
var nTableRadial = 1001;

var lookupTableLegendre = Array(nTable);
var lookupTableRadial = Array(nTableRadial);

var radialZeros = Array();

var legendreCoeff = Array(ellMax+1);

var tabulatedN = -1;
var tabulatedEll = -1;
var tabulatedM = -1;
var legendreZeros = Array();

var maxR = 1;
var deltaTheta = Math.PI/(nTable-1);
var delatR = maxR/(nTableRadial-1);


// Init tabulated coefficients for computation of Legendre polynomials
function initLegendreCoeffs(ell,m) {
    tabulatedEll = ell;
    tabulatedM = m;
    var absM = Math.abs(m);
    var ellEll1 = ell*(ell+1);
    legendreCoeff[0] = 1-2*(ell%2);
    console.log(''+ell+' '+legendreCoeff[0]);
    // No problem in using same array thanks to even/odd alternation in coefficients
    for (var iM = ell; iM>absM; iM--) {
        var denominator=Math.sqrt(ellEll1-iM*(iM-1));
        var kStart = (ell-iM)%2;
        if (kStart == 1)
            legendreCoeff[0] = 0;
        for (var k=kStart; k<=(ell-iM); k+=2) {
            if (k>0)
              legendreCoeff[k-1]+= k*legendreCoeff[k]/denominator;
            legendreCoeff[k+1] = -(k+2*iM)*legendreCoeff[k]/denominator;
        }
    }
    // Normalization so that maximum value is equal to one
    var maxValue = 0;
    for (var theta=0; theta<Math.PI/2; theta+=Math.PI/100)
        maxValue = Math.max(maxValue,Math.abs(computePLM(theta,ell,m)));
    for (var iM=0; iM<=ell; iM++) {
        legendreCoeff[iM]/=maxValue;
        // console.log('coeff['+iM+'] = '+legendreCoeff[iM]);
    }
}

// Legendre associated polynomial
function computePLM(theta,ell,m) {
    var u = Math.cos(theta);
    var absM=Math.abs(m);
    // To save time, we take advantage of the fact that non zero coefficients
    // have same parity as ell-m.
    // Also, we know that the degree of the polynomial iss ell-m.
    var sum = 0;
    var uPowJ;
    if ((ell-absM)%2 == 0)
      uPowJ = 1;
    else
      uPowJ = u;
    var u2 = u*u;
    for (var j = (ell-absM)%2; j<= ell-absM; j+=2, uPowJ*=u2)
      sum+= legendreCoeff[j]*uPowJ; // Calcule le terme aj u^j et l'ajoute
    return sum*Math.pow(Math.sin(theta), Math.abs(m));
}

// Legendre lookup table initialization
function initLookupTable(ell,m) {
    deltaTheta = Math.PI/(nTable-1);
    for (var i=0; i<nTable; i++) {
        var theta = Math.PI*i/(nTable-1);
        lookupTableLegendre[i] = computePLM(theta,ell,m);
    }
}

// Radial wavefunction lookup table initialization
function initLookupTableRadial(n,ell) {
    maxR = 6*n*n;
    deltaR = maxR/(nTableRadial-1);
    for (var i=0; i<nTableRadial; i++) {
        var r = i*deltaR;
        lookupTableRadial[i] = hydrogenRadialFunction(r,n,ell);
    }
}

// Get value of Legendre associated polynomial from lookup table
function PLM(theta,ell,m) {
    return getValueFromLookupTable(theta/Math.PI,lookupTableLegendre);
}

function findLegendreZeros(ell,m) {
    nZeros = ell - Math.abs(m);
    legendreZeros = Array(nZeros);
    iZero = 0;
    var currentSign = Math.sign(lookupTableLegendre[1]);
    for (var i=2; (i<lookupTableLegendre.length) && (iZero<nZeros/2); i++) {
        if (Math.sign(lookupTableLegendre[i])!=currentSign) { // Sign change
            legendreZeros[iZero] = (i-1+lookupTableLegendre[i-1]/(lookupTableLegendre[i-1]-lookupTableLegendre[i]))/(nTable-1);
            legendreZeros[nZeros-1-iZero] = 1-legendreZeros[iZero];
            iZero++;
            currentSign = -currentSign;
        }
    }
}

function findRadialZeros(n,ell) {
    nZeros = n - ell - 1;
    radialZeros = Array(nZeros);
    iZero = 0;
    var currentSign = Math.sign(lookupTableRadial[1]);
    for (var i=2; (i<nTableRadial) && (iZero<nZeros); i++) {
        if (Math.sign(lookupTableRadial[i])!=currentSign) { // Sign change
            radialZeros[iZero] = (i-1+lookupTableRadial[i-1]/(lookupTableRadial[i-1]-lookupTableRadial[i]))/(nTableRadial-1);
            iZero++;
            currentSign = -currentSign;
        }
    }
}

var laguerreCoeff = Array(nMax);

// Init tabulated coefficients for computation of Legendre polynomials
function initLaguerreCoeffs(n, ell) {
    console.log('Calculation of Laguerre polynomial coefficients')
    tabulatedN = n;
    tabulatedEll = ell;
    // Clear coefficient table
    for (var k=0; k<nMax; k++) {
        laguerreCoeff[k] = 0;
    }
    // Enforce n<= nMax and 0 <= ell <= n-1
    n = Math.min(n, nMax);
    ell = Math.min(ell, n-1);
    ell = Math.max(ell,0);
    var degree = n-ell;
    // Compute coefficients using eq. 11.26 of Basdevant/Dalibard
    laguerreCoeff[0] = 1;
    for (var k=0; k+1<degree; k++) // Equation 11.26 du BD
      laguerreCoeff[k+1] = -2*(1-(ell+k+1.)/n)/(k+1.)/(2*ell+k+2)*laguerreCoeff[k];
}

// Radial wavefunction of hydrogen
function hydrogenRadialFunction(r, n, ell) {
    if ((n!==tabulatedN)||(ell!==tabulatedEll))
        initLaguerreCoeffs(n,ell);
    var result = laguerreCoeff[0];
    var p = 1;
    for (var k=1; k<n-ell; k++) {
      p = p*r;
      result+=p*laguerreCoeff[k];
    }
    result*=Math.pow(r,ell)*Math.exp(-r/n);
    return result;
}

/*******************************************************************************
 * 
 * Basic functions
 * 
 ******************************************************************************/

// Convert wavelength (in nm) to rgb color
function wavelengthToRgb(wavelength) {
    r = 0;
    g = 0;
    b = 0;
    lambda = Math.min(wavelength,779.99);
    lambda = Math.max(lambda,400);
    lambda1 = 380, lambda2 = 400, lambda3 = 490, lambda4 = 510;
    lambda5 = 580, lambda6 = 645, lambda7 = 780;
    r = 0, g = 0, b = 0, s = 0;
    if (lambda>=lambda1)
      r = Math.max(0,Math.min(1,(lambda2-lambda)/(lambda2-lambda1)));
    if (lambda<lambda7)
      r +=Math.max(0,Math.min(1,(lambda-lambda4)/(lambda5-lambda4)));
    if (lambda<lambda2)
      g = 0; // No green
    else if (lambda<lambda3) {
      g += (lambda-lambda2)/(lambda3-lambda2);
    } else if (lambda<lambda5) {
      g += 1;
    } else if (lambda<lambda6)
      g += (lambda6-lambda)/(lambda6-lambda5);
    if (lambda<lambda1)
      b = 0; // No blue there
    else if (lambda<lambda3)
      b = 1;
    else if (lambda<lambda4)
      b = (lambda4-lambda)/(lambda4-lambda3);
    if (lambda<420)
      s = Math.max(0,.3+.7*(lambda-380)/(420-380));
    else if (lambda<700)
      s = 1;
    else
      s = Math.max(0,.3+.7*(780-lambda)/(780-700));
    r = Math.floor(r*s*255.99);
    g = Math.floor(g*s*255.99);
    b = Math.floor(b*s*255.99);
    return [r,g,b];
}

function complexToRgb(amplitude,phase) {
    r = 0;
    g = 0;
    b = 0;
    phase = phase/2/Math.PI;
    phase = phase + .5;  // To rotate color map by pi
    phase = phase - Math.floor(phase); // Value between 0 and 1
    phase = phase*6;
    remainder = phase-Math.floor(phase);
    eta1 = Math.pow(remainder,.6);// To make it smoother and broader
    eta2 = Math.pow(1-remainder,.6);
    switch(Math.floor(phase)) {
        case 0:
        case 6:
            r = 255;
            g = Math.floor(255*eta1);
            break;
        case 1:
            g = 255;
            r = Math.floor(255*eta2);
            break;
        case 2:
            g = 255;
            b = Math.floor(255*eta1);
            break;
        case 3:
            b = 255;
            g = Math.floor(255*eta2);
            break;
        case 4:
            b = 255;
            r = Math.floor(255*eta1);
            break;
        case 5:
            r = 255;
            b = Math.floor(255*eta2);
            break;
    }
    amplitude = amplitude;
    r = Math.floor(amplitude*r);
    g = Math.floor(amplitude*g);
    b = Math.floor(amplitude*b);
    return [r,g,b];
}

// Get value using provided lookup table,
// using linear interpolation between tabulated points
// Assumes that x=0 corresponds to first point of table
//          and x=1 corresponds to last point of table
function getValueFromLookupTable(x, table) {
    var n = table.length;
    var iFloat = x*(n-1);
    iFloat = Math.max(0,iFloat);
    var i = Math.floor(iFloat);
    var eta = iFloat-i;
    if (i<n-1)
        return table[i]*(1-eta)+table[i+1]*eta;
    else
        return table[n-1];
}

// Search for the floating index associated with specified target value
// using the lookup table assumed to vary monotically from table[0] to
// table[n-1].
function solveUsingTable(target, table, iMin = 0, iMax = -1, increasing = true) {
    var debug = false;
    if (debug)
      console.log("solveUsingTable "+target+" "+iMax+" "+increasing);
    if (iMax<0) {
        debug = false;
    }
    var n = table.length;
    if (iMax<0)
        iMax = n-1;
    var sign = 1;
    if (!increasing) {
        sign = -1;
        target = -target;
    }
    if (debug)
        console.log("sut : "+table[iMin]+" "+target+" "+table[iMax]+" "+sign);
    // First process out of bound cases
    if (target<=sign*table[iMin])
        return iMin;
    else if (target>=sign*table[iMax])
        return iMax;
    // Find closest index by dichotomy
    while (iMax>iMin+1) {
        if (debug)
            console.log("Dicho : "+iMin+" "+iMax+" "+table[iMin]+" "+target+" "+table[iMax]);
        var i = Math.round((iMin+iMax)/2);
        if (target<sign*table[i])
            iMax = i;
        else
            iMin = i;
    }
    // Concludes with a linear interpolation
    target = sign*target;
    eta = (target-table[iMin])/(table[iMax]-table[iMin]);
    return iMin+eta;
}

/*******************************************************************************
 * 
 * Linear algebra
 * 
 ******************************************************************************/

// Square matrix initialized with zeroes
function matrix(nRow, nCol) {
    var m = Array(nRow);
    for (var i=0; i<nRow; i++)
        m[i] = Array(nCol).fill(0);
    return m;
}

// Square matrix initialized with zeroes
function squareMatrix(n) {
    return matrix(n,n);
}

// Tridiagonal matrix
function tridiag(n, value=1) {
    var m = squareMatrix(n);
    for (var i=0; i<n-1; i++) {
        m[i][i+1] = value;
        m[i+1][i] = value;
    }
    return m;
}

// Used by the diagonalization procedure, eig
function diagRotate(a, i, j, k, l, s, tau)
  {
    var g,h;
    g=a[i][j];
    h=a[k][l];
    a[i][j]=g-s*(h+g*tau);
    a[k][l]=h+s*(g-h*tau);
  }

 // Sort eigenvalues (and associated eigenvectors) by increasing values of eigenvalue
 function sortEigenvalues(eigenvalue, eigenvector)
  {
    var n = eigenvalue.length;
    var index=Array(n).fill(0);
    for (var i=0; i<n; i++)
    {
      var j;
      for (j=0; (j<i) && (eigenvalue[i]>eigenvalue[index[j]]); j++) {
      }
      for (var k=i; k>j; k--)
        index[k]=index[k-1];
      index[j]=i;
    }
    var newVap=Array(n);
    var newVep=Array(n);
    for (var i=0; i<n; i++)
      newVap[i]=eigenvalue[index[i]];
    for (var i=0; i<n; i++)
      eigenvalue[i]=newVap[i];
    for (var i=0; i<n; i++) {
      newVep[i] = Array(n);
      for (var j=0; j<n; j++)
        newVep[i][j]=eigenvector[i][index[j]];
    }
    for (var i=0; i<n; i++)
    for (var j=0; j<n; j++)
      eigenvector[i][j]=newVep[i][j];
  }


// Compute eigenvalues and eigenvectors of a real symmetric matrix
// Based on Numerical Recipes
// @param matrix        n x n matrix
// @param eigenvalue    n vector
// @param eigenvector   n x n matrix
function diagonalize(matrix) {
    var nRot=0;
    var j,iq,ip,i;
    var tresh,theta,tau,t,sm,s,h,g,c;
    var n = matrix.length;
    var b = Array(n);
    var z = Array(n);
    
    var eigenvalue = Array(n);
    var eigenvector = Array(n);
        
    // Initialize eigenvector matrix to the identity
    for (var ip=0; ip<n; ip++)
    {
      eigenvector[ip] = Array(n);
      for (var iq=0; iq<n; iq++)
        eigenvector[ip][iq]=0;
      eigenvector[ip][ip]=1;
    }
    for (var i1=0; i1<n; i1++)
        for (var i2=0; i2<n; i2++)
            eigenvector[i1][i2] = 1;
    
    alpha = [[1, 2, 3],[4, 5, 6], [7, 8, 9]];

    // b is initialized to matrix diagonal elements
    for (var ip=0; ip<n; ip++)
    {
      b[ip]=eigenvalue[ip]=matrix[ip][ip];
      z[ip]=0;
    }
    nRot=0;

    for (var iPass=0; iPass<50; iPass++)
    {
      // Compute the sum of diagonal elements
      sm=0;
      for (var ip=0; ip<n-1; ip++)
      for (var iq=ip+1; iq<n; iq++)
        sm+=Math.abs(matrix[ip][iq]);
      if (sm==0) {
        sortEigenvalues(eigenvalue, eigenvector);
        return [eigenvalue,eigenvector];
      }
      if (iPass<4)
        tresh=0.2*sm/(n*n);
      else
        tresh=0;
      for (var ip=0; ip<n-1; ip++)
      {
        for (var iq=ip+1; iq<n; iq++)
        {
          g=100*Math.abs(matrix[ip][iq]);
          if ((iPass>4)&& (Math.abs(eigenvalue[ip]+g) == Math.abs(eigenvalue[ip]))
                       && (Math.abs(eigenvalue[iq]+g) == Math.abs(eigenvalue[iq])))
            matrix[ip][iq]=0;
          else if (Math.abs(matrix[ip][iq])>tresh)
          {
            h=eigenvalue[iq]-eigenvalue[ip];
            if ((Math.abs(h)+g)==Math.abs(h))
              t=matrix[ip][iq]/h;
            else
            {
              theta=0.5*h/matrix[ip][iq];
              t=1/(Math.abs(theta)+Math.sqrt(1+theta*theta));
              if (theta<0)
                t=-t;
            }
            c=1/Math.sqrt(1+t*t);
            s=t*c;
            tau=s/(1+c);
            h=t*matrix[ip][iq];
            z[ip]-=h;
            z[iq]+=h;
            eigenvalue[ip]-=h;
            eigenvalue[iq]+=h;
            matrix[ip][iq]=0;
            for (j=0; j<=ip-1; j++)
              diagRotate(matrix,j,ip,j,iq,s,tau);
            for (j=ip+1; j<=iq-1; j++)
              diagRotate(matrix,ip,j,j,iq,s,tau);
            for (j=iq+1; j<n; j++)
              diagRotate(matrix,ip,j,iq,j,s,tau);
            for (j=0; j<n; j++)
              diagRotate(eigenvector,j,ip,j,iq,s,tau);
          }
          nRot++;
        }
      }
      for (ip=0; ip<n; ip++)
      {
        b[ip]+=z[ip];
        eigenvalue[ip]=b[ip];
        z[ip]=0;
      }
    }
    sortEigenvalues(eigenvalue, eigenvector);
    return [eigenvalue,eigenvector];
  }


/*******************************************************************************
 * 
 * Basic Math
 * 
 ******************************************************************************/

function sqr(value) {
    return value*value;
}

// Return sum of array elements
function sum(array) {
    var result = 0;
    for (var i=0; i<array.length; i++)
        result += array[i];
    return result;
}

// Return minimum value of array
function min(array) {
    result = array[0];
    for (var i = 1; i<array.length; i++)
        result = Math.min(result,array[i]);
    return result;
}

// Return maximum value of array
function max(array) {
    result = array[0];
    for (var i = 1; i<array.length; i++)
        result = Math.max(result,array[i]);
    return result;
}

// Return maximum absolute value of an array for i in [i1,i2[
function maxAbs(array, i1 = 0, i2 = -1) {
    if (i2<0)
        i2 = array.length;
    result = Math.abs(array[i1]);
    for (var i = i1+1; i<i2; i++)
        result = Math.max(result,Math.abs(array[i]));
    return result;
}

// Return index associated with maximum absolute value of an array for i in [i1,i2[
function indexMaxAbs(array, i1 = 0, i2 = -1) {
    if (i2<0)
        i2 = array.length;
    var maxValue = Math.abs(array[i1]);
    result = i1;
    for (var i = i1+1; i<i2; i++) {
        if (Math.abs(array[i])>maxValue) {
            result = i;
            maxValue = Math.abs(array[i]);
        }
    }
    return result;
}

// Return index associated with minimum distance to specified value
function indexMinDistance(array, value) {
    var result = 0;
    var minDistance = Math.abs(array[0]-value);
    for (var i=1; i<array.length; i++) {
        if (Math.abs(array[i]-value)<minDistance) {
            result = i;
            minDistance = Math.abs(array[i]-value);
        }
    }
    return result;
}

// Compute factorial using table
var factorialTable = new Array(100);
function initFactorialTable() {
    factorialTable[0] = 1;
    for (var i=1; i<factorialTable.length; i++)
        factorialTable[i] = i*factorialTable[i-1];
}
initFactorialTable();
function factorial(n) {
    if (n<factorialTable.length)
        return factorialTable[n];
    else {
        factorialTable = new Array(n+1);
        initFactorialTable();
        return factorialTable[n];
    }
}

// Return array [0,1, ..., n-1]
function range(n1, n2 = 0) {
    if (n2 === 0) {
        n2 = n1;
        n1 = 0;
    }
    var result = Array(n2-n1);
    for (var i=0; i<n2-n1; i++) {
        result[i] = n1+i;
    }
    return result;
}

// Format an integer number with leading zeros
function format(n, length) {
    return ("000000"+n).slice(-length);
}

class Point {
    constructor(x = 0, y = 0) {
        if (x instanceof Point) {
            this.x = x.x;
            this.y = x.y;
        } else {
            this.x = x;
            this.y = y;
        }
    }
    
    // Distance to origin
    length() {
        return Math.sqrt(sqr(this.x)+sqr(this.y));
    }
    
    distanceTo(p) {
        return Math.sqrt(sqr(p.x-this.x)+sqr(p.y-this.y));
    }
}

class Segment {
    constructor(a, b) {
        this.a = a;
        this.b = b;
        this.v = new Point(b.x-a.x, b.y-a.y);
    }
    
    distanceTo(p) {
        const segLength = this.v.length();
        // Handle case where length of segment is zero
        if (segLength==0)
            return this.a.distanceTo(p);
        // Compute the location of the projection H of P onto the line AB
        const t = ((p.x - this.a.x) * this.v.x + (p.y - this.a.y) * this.v.y) / sqr(segLength);
        if (t < 0) { // H is outside the segment, closer to A
            return this.a.distanceTo(p);
        } else if (t > 1) { // H is outside the segment, closer to B
            return this.b.distanceTo(p);
        } else { // H is inside the segment
            // compute H coordinates
            const h = new Point(this.a.x + t * this.v.x, this.a.y + t*this.v.y);
            return h.distanceTo(p);
        }
    }    
}

// Complex number
class Complex {
    constructor(re = 0, im = 0) {
        if (re instanceof Complex) {
            this.re = re.re;
            this.im = re.im;
        } else {
          this.re = re;
          this.im = im;
      }
    }
    sqrabs() {
        return this.re*this.re+this.im*this.im;
    }
    abs() {
        return Math.sqrt(this.sqrabs());
    }
    angle() {
        return Math.atan2(this.im,this.re);
    }
    add(a) {
        return Complex.add(this,a);
    }
    _add(a) {
        this.re += a.re;
        this.im += a.im;
    }
    _mul(z) {
      if (z instanceof Complex) {
          var r = this.re;
          this.re = this.re*z.re-this.im*z.im;
          this.im = r*z.im+this.im*z.re;
      } else {
          this.re *= z;
          this.im *= z;
      }
    }
    conj() {
        return new Complex(this.re,-this.im);
    }
    exp() {
        return Complex.fromPolar(Math.exp(this.re),this.im);
    }
    toString() {
        return this.re.toFixed(3)+"+"+this.im.toFixed(3)+"i";
    }
    static add(a,b) {
        return new Complex(a.re+b.re, a.im+b.im);
    }
    static mul(a,b) {
        return new Complex(a.re*b.re-a.im*b.im,a.re*b.im+a.im*b.re);
    }
    static fromPolar(amplitude, phase) {
        return new Complex(amplitude*Math.cos(phase),amplitude*Math.sin(phase));
    }
}

// Real array
class XArray {
    constructorFromArray(array) {
        this.data = new Array(array.length);
        for (var i=0; i<array.length; i++)
            this.data[i] = array[i];
        this.n = array.length;
    }
    constructor(n) {
        if (n instanceof XArray) {
            this.data = new Array(n.data.length);
            this.n = n.data.length;
            for (var i=0; i<this.n; i++)
                this.data[i] = n.data[i];
        }
        else if (n instanceof Array)
            this.constructorFromArray(n);
        else {
            this.data = new Array(n);
            this.n = n;
            for (var i=0; i<n; i++)
                this.data[i] = 0;
        }
    }
    linspace(x1,x2, endpoint = true) {
        var n = this.data.length;
        var denominator;
        if (endpoint)
            denominator = n-1;
        else
            denominator = n;
        for (var i=0; i<n; i++)
            this.data[i] = x1+(x2-x1)*i/denominator;
    }
    fill(value) {
        for (var i=0; i<this.data.length; i++)
            this.data[i] = value;
    }
    size() {
        return this.data.length;
    }
    norm2() {
        var result = 0;
        for (var i=0; i<this.size(); i++)
            result += this.data[i]*this.data[i];
        return result;
    }
    norm() {
        return Math.sqrt(this.norm2());
    }
    _mul(value) {
        var n = this.data.length;
        if (value instanceof XArray) {
            for (var i=0; i<this.size(); i++)
                this.data[i] *= value.data[i];
        } else {
            for (var i=0; i<this.n; i++)
                this.data[i] *= value;
        }
    }
    _add(value) {
        if (value instanceof XArray) {
            for (var i=0; i<this.size(); i++)
                this.data[i]+=value.data[i];
        } else {
            for (var i=0; i<this.size(); i++)
                this.data[i]+=value;
        }
    }
    _sub(value) {
        if (value instanceof XArray) {
            for (var i=0; i<this.size(); i++)
                this.data[i]-=value.data[i];
        } else {
            for (var i=0; i<this.size(); i++)
                this.data[i]-=value;
        }
    }
    _sqr() {
        var n = this.data.length;
        for (var i=0; i<n; i++)
            this.data[i] *= this.data[i];
    }
    sqr() {
        var result = new XArray(this);
        result._sqr();
        return result;
    }
    _sqrt() {
        var n = this.data.length;
        for (var i=0; i<n; i++)
            this.data[i] = Math.sqrt(this.data[i]);
    }
    log10() {
        var result = new XArray(this);
        result._log10();
        return result;
    }
    _log10() {
        for (var i=0; i<this.data.length; i++)
            this.data[i] = Math.log10(this.data[i]);
    }
    flip() {
        var result = new XArray(this.n);
        for (var i = 0; i<n; i++)
            result.data[i] = this.data[n-i-1];
        return result;
    }
    // Return average value ponderated by distribution
    avg(distribution = null) {
        if (distribution === null)
            return this.sum()/this.n;
        var array = new XArray(this);
        array._mul(distribution);
        return array.sum()/distribution.sum();
    }
    sum() {
        var result = 0;
        for (var i=0; i<this.n; i++)
            result += this.data[i];
        return result;
    }
    max() {
        var result = this.data[0];
        for (var i=1; i<this.n; i++)
            result = Math.max(result,this.data[i]);
        return result;
    }
    min() {
        var result = this.data[0];
        for (var i=1; i<this.n; i++)
            result = Math.min(result,this.data[i]);
        return result;
    }
    toString() {
        var s = '[';
        for (var i=0;i<this.data.length;i++) {
            if (i>0)
                s += ", "
            s += this.data[i].toFixed(3);
        }
        s += ']';
        return s;
    }
    
    // Fill array with expression such as 'sin(x)/x'
    // where x is the previous array value
    eval(s) {
        var n = this.data.length;
        for (var i=0; i<n; i++) {
            var x = this.data[i];
            var value = x.toString();
            this.data[i] = eval(s.replace(/x/gi,value));
        }
        
    }
    // Return XArray filled with n1,n1+1, ..., n2-1 (if n2>0), or 0, 1, ... n1-1 (if n2==0) 
    static range(n1, n2 = 0) {
        return new XArray(range(n1,n2));
    }
}

// Complex array
class ZArray{
    constructor(object) {
        if (object instanceof Array)
            object = new XArray(object);
        if (object instanceof XArray) { // Complex array with zero imaginary part
            this.n = object.size();
            this.data = new Array(2*this.n);
            for (var i=0; i<this.n; i++) {
                this.data[i] = object.data[i];
                this.data[i+this.n] = 0;
            }
        } else if (object instanceof ZArray) { // Copy constructor
            this.n = object.size();
            this.data = new Array(2*this.n);
            for (var i=0; i<2*this.n; i++) 
                this.data[i] = object.data[i];
        } else { // Zero array of specified size
            this.n = object;
            this.data = new Array(2*this.n);
            for (var i=0; i<2*this.n; i++)
                this.data[i] = 0;
        }
    }
    
    size() {
        return this.n;
    }
    _add(object) {
        if (object instanceof ZArray) {
            for (var i=0; i<2*object.size(); i++)
                this.data[i] += object.data[i];
        } else
            console.log('This version of ZArray.add is not implemented yet...');
    }
    
    _addPhase(phase) {
        var expFactor = new ZArray(phase);
        expFactor._mul(new Complex(0,1));
        expFactor._exp();
        this._mul(expFactor);
    }
    _mul(object) {
        if (object instanceof Complex) {
            for (var i=0; i<this.size(); i++) {
                var y = this.re(i)*object.im+this.im(i)*object.re;
                this.data[i] = this.re(i)*object.re-this.im(i)*object.im;
                this.data[i+this.size()] = y;
            }
        } else if (object instanceof ZArray) {
            for (var i=0; i<this.size(); i++) {
                var y = this.re(i)*object.im(i)+this.im(i)*object.re(i);
                this.data[i] = this.re(i)*object.re(i)-this.im(i)*object.im(i);
                this.data[i+this.size()] = y;
            }
        } else {
            for (var i=0; i<this.size()*2; i++)
                this.data[i] *= object;
        }
    }
    _exp() {
        for (var i=0; i<this.size(); i++) {
            this.setValue(i,this.getValue(i).exp());
        }
    }
    
    _rotate(shift) {
        var buffer = new Array(2*this.n);
        for (var i=0; i<this.n; i++) {
            buffer[i] = this.data[(i+shift+this.n)%this.n];
            buffer[i+this.n] = this.data[(i+shift+this.n)%this.n+this.n];
        }
        for (var i=0; i<2*this.n; i++)
            this.data[i] = buffer[i];
    }
    
    _fftshift() {
        this._rotate(this.n >> 1);
    }

    _fft(sign = 1) {
        sign = -sign;
        var tempr = 0;
        var tempi = 0;
        var theta = 0; // Angle 2 pi nu t
        var j=0;
        var n = this.n;
        for (var i=0; i<n-1; i++) {
          if (j>i) { // swap [i] and [j]
            tempr = this.data[j]; this.data[j] = this.data[i]; this.data[i] = tempr;
            tempi = this.data[j+this.n]; this.data[j+n] = this.data[i+n]; this.data[i+n] = tempi;
          }
          var m = n >> 1;
          while (m>=1 && j>m-1) {
            j-= m;
            m = m >> 1;
          }
          j+=m;
        }
        var mmax = 1;
        while (n > mmax) {
          var iStep = mmax << 1;
          theta = sign*Math.PI/mmax;
          var wtemp = Math.sin(theta/2);
          var wpr = -2*wtemp*wtemp;
          var wpi = Math.sin(theta);
          var wr = 1;
          var wi = 0;
          for (var m=0; m<mmax; m++) {
            for (var i=m; i<n; i+=iStep) {
              j = i+mmax;
              tempr = wr*this.data[j]-wi*this.data[j+n];
              tempi = wr*this.data[j+n]+wi*this.data[j];
              this.data[j] = this.data[i] - tempr;
              this.data[j+n] = this.data[i+n] - tempi;
              this.data[i] += tempr;
              this.data[i+n] += tempi;
            }
            var temp = wr;
            wr = wr*wpr-wi*wpi+wr;
            wi = wi*wpr+temp*wpi+wi;
          }
          mmax = iStep;
        }
    }
    
    _ft() {
        this._fftshift();
        this._fft();
        this._fftshift();
    }
    
    ft() {
        var output = new ZArray(this);
        output._ft();
        return output;
    }
    
    _ift() {
        this._fftshift();
        this._fft(-1);
        this._mul(1/this.n);
        this._fftshift();
    }
    
    ift() {
        var output = new ZArray(this);
        output._ift();
        return output;
    }
    
    abs2() {
        var result = new XArray(this.size());
        for (var i=0; i<this.size(); i++)
            result.data[i] = this.re(i)*this.re(i)+this.im(i)*this.im(i);
        return result;
    }
    
    abs() {
        var result = new XArray(this.size());
        for (var i=0; i<this.size(); i++)
            result.data[i] = Math.sqrt(this.re(i)*this.re(i)+this.im(i)*this.im(i));
        return result;
    }
    
    real() {
        var result = new XArray(this.size());
        for (var i=0; i<this.size(); i++)
            result.data[i] = this.re(i);
        return result;
    }

    re(i) {
        return this.data[i];
    }
    
    im(i) {
        return this.data[i+this.size()];
    }
    getValue(i) {
        return new Complex(this.re(i),this.im(i));
    }
    setValue(i, value) {
        if (value instanceof Complex) {
            this.data[i] = value.re;
            this.data[i+this.size()] = value.im;
        } else {
            this.data[i] = value;
            this.data[i+this.size()] = 0;
        }
    }
    // Return <psi|psi>
    norm2() {
        var result = 0;
        for (var i=0; i<this.size()*2; i++)
            result += this.data[i]*this.data[i];
        return result;
    }
    
    toString() {
        var s = '[';
        for (var i=0;i<this.n;i++) {
            if (i>0)
                s += ", "
            s += this.getValue(i).toString();
        }
        s += ']';
        return s;
    }
    clear() {
        for (var i=0; i<this.size()*2; i++)
            this.data[i] = 0;
    }
    static hermitianProduct(a,b) {
        return dotProduct(a.conj(),b);
    }
    static hermitianProduct(a,b) {
        var result = new Complex();
        for (var i=0; i<a.size(); i++)
            result._add(Complex.mul(a.getValue(i).conj(),b.getValue(i)));
        return result;
    }
    
}

// Complex matrix
class ZMatrix {
    constructor(n) {
        this.n = n;
        this.data = new Array(2*n*n);
        this.clear();
    }
    clear() {
        for (var i=0; i<this.n*this.n*2; i++)
            this.data[i] = 0;
    }
    setValue(row, col, value) {
        this.data[(col*this.n+row)*2] = value.re;
        this.data[(col*this.n+row)*2+1] = value.im;
    }
    getValue(row, col) {
        return new Complex(this.data[(col*this.n+row)*2],this.data[(col*this.n+row)*2+1]);
    }
    _add(zMatrix) {
        for (var i=0; i<this.n*this.n*2; i++)
            this.data[i] += zMatrix.data[i];
    }
    mul(vector) {
        if (vector instanceof ZArray) {
            var result = new ZArray(vector.size());
            for (var row=0; row<this.n; row++) {
                for (var col = 0; col<this.n; col++) {
                    var product = Complex.mul(this.getValue(row,col),vector.getValue(col));
                    result.setValue(row,result.getValue(row).add(product));
                }
            }
            return result;
        } else if (vector instanceof ZMatrix) {
            var result = new ZMatrix(this.n);
            var mat = vector;
            for (var row = 0; row<this.n; row++) {
                for (var col = 0; col<this.n; col++) {
                    var sum = new Complex();
                    for (var k=0; k<this.n; k++)
                        sum._add(Complex.mul(this.getValue(row,k),mat.getValue(k,col)));
                    result.setValue(row,col,sum);
                }
            }
            return result;
            
        } else if (vector instanceof Complex) {
            var result = new ZMatrix(this.n);
            for (var row = 0; row<this.n; row++)
                for (var col = 0; col<this.n; col++)
                    result.setValue(row,col,Complex.mul(this.getValue(row,col),vector));
            return result;
        } else
            console.log('Not implemented');
    }
    dagger() {
        var result = new ZMatrix(this.n);
        for (var row = 0; row<this.n; row++) {
            for (var col = 0; col<this.n; col++) {
                result.setValue(row,col,this.getValue(col,row).conj());
            }
        }
        return result;
    }
    toString() {
        var s = '';
        for (var row=0; row<this.n; row++) {
            for (var col=0; col<this.n; col++) {
                s += this.getValue(row,col).toString()+" ";
            }
            s+='\n';
        }
        return s;
    }

    static diag(complexArray, shift) {
        if (complexArray instanceof XArray)
            complexArray = new ZArray(complexArray); // make real array a complex array
        var result = new ZMatrix(complexArray.size()+Math.abs(shift));
        for (var i=0; i<complexArray.size(); i++) {
            var row, col;
            if (shift>=0) {
                row = i;
                col = i+shift; }
            else {
                row = i-shift;
                col = i;
            }
            result.setValue(row,col,complexArray.getValue(i));
        }
        return result;
    }
}

/*******************************************************************************
 * 
 * Graphics
 * 
 ******************************************************************************/

const HOTSPOT_XY = 0;
const HOTSPOT_Y = 1;
const HOTSPOT_X = 2;
const HOTLINE_X = 3;
const HOTLINE_Y = 4;
const HOTSEGMENT = 5;
const HOTSPOT_DISABLED = -1;

function newImage(src) {
    var result = new Image();
    result.src = src;
    return result;
}

/**
 * Non-visual class for managing ticks and labels of a graduated axis.
 * Useful for the horizontal or vertical axis of a graph or for a bar graph axis.
 *
 *          x1                                 x2
 *          |<--------------------------------->|
 *       |     |     |     |     |     |     |     |
 *      10    20    30    40    50    60    70    80
 *    outerX1                                    outerX2
 *
 * The example above corresponds to x1 = 14.32 and x2 = 78.32. In this case,
 * nTicks equals 8.
 *
 * Future developments: minor ticks, scientific notation, log scale
 *
 */
class Ticker {
  // ========================== PROPERTIES =========================
  /** Horizontally oriented axis */
  HORIZONTAL = 0;
  /** Vertically oriented axis */
  VERTICAL = 1;

  /**
   * Axis orientation. This parameter must be specified because, in the case
   * of a horizontally oriented axis, the ticks need to be spaced more
   * apart depending on the number of characters in the label.
   * In contrast, for a vertically oriented axis, tick spacing depends only
   * on the height of each character, not their count.
   */
  orientation = this.HORIZONTAL;

  /** Scaler needed for pixel-to-user-coordinate mapping */
  scaler = null;
  
  /** Lower bound of the Ticker */
  x1 = 0;

  /** Upper bound of the Ticker */
  x2 = 0;

  /** External lower bound */
  outerX1 = 0;

  /** External upper bound */
  outerX2 = 0;

  /** Number of ticks */
  nTicks = 0;

  /** Maximum number of ticks allowed */
  maxTicks = 13;

  /** Additional significant digits for certain tick spacings */
  additionalDigits = 2;

  /** Character size. Width for a horizontal axis, height for a vertical axis.  */
  charSize = 10;

  // ================================= CONSTRUCTORS =====================
  constructor(x1, x2, scaler) {
    this.x1 = x1;
    this.x2 = x2;
    this.scaler = scaler;
    this.update();
  }

  /**
   * Access the user coordinate of the i-th tick
   */
  getX(i) {
    return this.outerX1 + (this.outerX2 - this.outerX1) * i / (this.nTicks - 1);
  }

  /**
   * Test if the i-th tick is visible
   */
  isVisible(i) {
    const x = this.getX(i);
    const epsilon = Math.abs(this.x2 - this.x1) / 1000;
    return x > Math.min(this.x1, this.x2) - epsilon &&
           x < Math.max(this.x1, this.x2) + epsilon;
  }

  /**
   * Access the pixel coordinate of the i-th tick
   */
  getI(i) {
      const x = this.getX(i);
      return (x-this.scaler[1])/this.scaler[0];
  }

  /**
   * Access the label of the i-th tick
   */
  getTickLabel(i) {
    let x = this.getX(i);
    let tickSize = Math.abs(this.outerX2 - this.outerX1) / (this.nTicks - 1);

    let nDigits = -Math.floor(Math.log10(tickSize)) + this.additionalDigits;
    nDigits = Math.max(0, nDigits);

    const fDigits = Math.pow(10, nDigits);
    x = Math.round(x * fDigits) / fDigits;

    return x.toFixed(nDigits);
  }

  /**
   * Calculate the number of ticks for this Ticker. Must be manually called
   * whenever Scaler, x1, or x2 changes.
   */
  update() {
    const steps = [0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 2.5, 5, 10];
    const delta=(this.x2-this.x1);
    var result=11;
    var xx1=this.x1+delta/1000;
    var xx2=this.x2-delta/1000;
    var r=Math.pow(10,Math.log(Math.abs(delta))/Math.log(10)-Math.floor((Math.log(Math.abs(delta))-Math.log(1.2))/Math.log(10)));    // was 1.2
    var rStep;
    var bestIS=0;
    var bestDelta=10*Math.abs(xx2-xx1);
    for (var iS=0; iS<10; iS++) {
        rStep=steps[iS]*delta/r;
        this.outerX1=Math.floor(xx1/Math.abs(rStep))*Math.abs(rStep);
        this.outerX2=Math.floor(xx2/Math.abs(rStep))*Math.abs(rStep);
        if (delta>0)
            this.outerX2=this.outerX2+rStep;
        else
            this.outerX1=this.outerX1-rStep;
        result=Math.floor(Math.abs((this.outerX2-this.outerX1)/rStep+0.2)+1);
        if (result<=this.maxTicks) {
            if (Math.abs(this.outerX2-this.outerX1)<bestDelta)
            {
              bestIS=iS;
              bestDelta=Math.abs(this.outerX2-this.outerX1);
            }
        }
    }
    rStep=steps[bestIS]*delta/r;
    if (bestIS==3 || bestIS==7) // 0.25 ou 2.5
      this.additionalDigits = 1;
    else
      this.additionalDigits = 0;
    this.outerX1=Math.floor(xx1/Math.abs(rStep))*Math.abs(rStep);
    this.outerX2=Math.floor(xx2/Math.abs(rStep))*Math.abs(rStep);
    if (delta>0)
      this.outerX2 += rStep;
    else
      this.outerX1 -= rStep;
    this.nTicks =Math.floor(0.5+Math.abs((this.outerX2-this.outerX1)/rStep+0.2)+1);
  }
}


class Graphix {
    constructor(canvas, x1, x2, y1, y2, margin = 0) {
        this.context = canvas.getContext("2d");
        this.width = canvas.width;
        this.height = canvas.height;
        // Set margins
        if (Array.isArray(margin)) {
            if (margin.length == 4) {
                this.leftMargin = margin[0];
                this.rightMargin = margin[1];
                this.topMargin = margin[2];
                this.bottomMargin = margin[3];
            } else {
                this.leftMargin = margin[0];
                this.rightMargin = margin[0];
                this.topMargin = margin[1];
                this.bottomMargin = margin[1];
            }
        } else {
            this.leftMargin = margin;
            this.rightMargin = margin;
            this.topMargin = margin;
            this.bottomMargin = margin;
        }
        this.scaler = Graphix.buildScaler(this.width-this.leftMargin-this.rightMargin,
                                          this.height-this.topMargin-this.bottomMargin,
                                          x1,x2,y1,y2);
        this.hotspotArray = new Array();
        this.hotspotType = new Array();
        this.currentIndex = -1;
        this.hotspotStatus = 0;
        this.hotspotMoved = false;
        this.hotspotStatusChanged = false;
        this.logStyle = 0;
        this.setFontSize(20);
    }
    
    // Plot styles
    static SOLID = 0;  // Standard graph with solid lines
    static FILLED = 1; // Graph with filled areas down to x axis
    static BAR = 2;    // Bar graph
    static FILLED_COMPLEX = 3; // Filled graph for complex data with color-coded phase
    static SYMBOLS = 4;
    
    // Plot orientation
    static HORIZONTAL = 0; // x axis horizontal (left to right), y axis vertical (bottom to up)
    static VERTICAL = 1; // x axis vertical (bottom to up), y axis horizontal (left to right)
    
    static LINEAR = 0;
    static SEMILOGX = 1;
    static SEMILOGY = 2;
    
    static GREEN_LIGHT = 0;
    static YELLOW_LIGHT = 1;
    static RED_LIGHT = 2;
   
    static BOTTOM = 0;
    static MIDDLE = 1;
    static TOP = 2;
    static LEFT = 0 << 2;
    static CENTER = 1 << 2;
    static RIGHT = 3 << 2;
    
    setScale(x1, x2, y1, y2) {
        this.scaler = Graphix.buildScaler(this.width-this.leftMargin-this.rightMargin,
                                          this.height-this.topMargin-this.bottomMargin,
                                          x1,x2,y1,y2);
    }

    setStrokeStyle(value) {
        this.context.strokeStyle = value;
    }
    
    setFillStyle(value) {
        this.context.fillStyle = value;
    }
    
    setLineWidth(value) {
        this.context.lineWidth = value;
    }
    
    setFont(font) {
        this.context.font = font;
    }
    
    setFontSize(fontSize) {
        this.fontSize = fontSize;
        this.context.font = fontSize + "px Times";
    }
    
    clear() {
        this.context.clearRect(0, 0, this.width, this.height);
    }
    
    beginPath() {
        this.context.beginPath();
    }

    moveTo(x,y) {
        this.context.moveTo(this.xPixel(x),this.yPixel(y));
    }
    
    lineTo(x,y) {
        this.context.lineTo(this.xPixel(x),this.yPixel(y));
    }
    
    plotDisk(x,y,radius) {
        this.context.beginPath();
        this.context.arc(this.xPixel(x), this.yPixel(y), radius, 0, Math.PI*2);
        this.context.fill();
    }
    
    plotDiskPixel(x,y,radius) {
        this.context.beginPath();
        this.context.arc(x, y, radius, 0, Math.PI*2);
        this.context.fill();
    }
    
    // Draw color code for plotting complex numbers
    plotComplexDisk(x,y,radius) {
        for (var i=0; i<10*radius; i++) {
            var phi = (2*Math.PI*i)/10/radius;
            this.beginPath();
            this.setStrokeStyle('rgb('+complexToRgb(1,phi)+')');
            this.context.moveTo(x,y);
            this.context.lineTo(x+Math.round(Math.cos(phi)*radius),y-Math.round(Math.sin(phi)*radius));
            this.stroke();
        }
    }
    
    plotBar(x,y,scaling) {
        this.barWidth = Math.max((this.xPixel(1)-this.xPixel(0))*0.8,1);
        this.context.fillStyle = this.context.strokeStyle;
        for (var i=0; i<x.length; i++) { // nBasis
            var xp = this.xPixel(i);
            if (scaling*y[i]>0) {
                var yp = this.yPixel(scaling*y[i]);
                var h = this.yPixel(0)-yp;
            } else {
                var yp = this.yPixel(0);
                var h = this.yPixel(scaling*[y[i]])-yp;
            }
            this.context.fillRect(xp-this.barWidth/2,yp,this.barWidth,h);
        }
    }

    plotBarVertical(x,y,scaling) {
        this.barWidth = Math.max(Math.abs(this.yPixel(1)-this.yPixel(0))*0.8,1);
        this.context.fillStyle = this.context.strokeStyle;
        for (var i=0; i<nBasis; i++) {
            var y = this.yPixel(i);
            if (scaling*coeff[i]>0) {
                var x = this.xPixel(scaling*coeff[i]);
                var h = this.xPixel(0)-x;
            } else {
                var x = this.xPixel(0);
                var h = this.xPixel(scaling*[coeff[i]])-x;
            }
            this.context.fillRect(x,y-this.barWidth/2,h,this.barWidth);
        }
    }
    
    // Clip the graph to the axes box
    clipToAxes() {
        this.beginPath();
        this.context.rect(this.leftMargin, this.topMargin,
                          this.width - this.leftMargin - this.rightMargin,
                          this.height - this.topMargin - this.bottomMargin);
        this.context.clip();
    }
    
    drawAxes(xLabel = '', yLabel = '', drawXTicks = true, drawYTicks = true) {
        this.setStrokeStyle("#000000");
        this.beginPath();
        this.context.rect(this.leftMargin, this.topMargin,
                          this.width - this.leftMargin - this.rightMargin+1,
                          this.height - this.topMargin - this.bottomMargin+1);
        this.context.stroke();
        // Draw horizontal label
        this.drawText(xLabel,this.leftMargin + (this.width - this.leftMargin - this.rightMargin)/2,this.height,Graphix.BOTTOM | Graphix.CENTER);
        if (drawXTicks) { // Draw ticks and tick labels
            var xTicker = new Ticker(this.xUser(this.leftMargin), this.xUser(this.width-this.rightMargin), [this.scaler[0], this.scaler[1]]);
            for (var i=0; i<xTicker.nTicks; i++) {
              if (xTicker.isVisible(i)) {
                const ix = xTicker.getI(i) + this.leftMargin;
                const iy = this.height - this.bottomMargin;
                this.drawVerticalLine(ix, iy, iy-5);
                this.drawText(xTicker.getTickLabel(i),ix,iy+3,Graphix.TOP | Graphix.CENTER);
              }
            }
        }
        if (drawYTicks) {
            var yTicker = new Ticker(this.yUser(this.height-this.bottomMargin), this.yUser(this.topMargin), [this.scaler[2], this.scaler[3]]);
            const ix = this.leftMargin;
            for (var i=0; i<yTicker.nTicks; i++) {
              if (yTicker.isVisible(i)) {
                const iy = yTicker.getI(i) + this.topMargin;
                this.drawHorizontalLine(ix, ix+6, iy);
              }
              if (this.logStyle & Graphix.SEMILOGY) {
                  for (var j=2; j<=9; j++) {
                      const iy = this.yPixel(yTicker.getX(i)+Math.log10(j));
                      if ((iy>this.topMargin)&&(iy<this.width-this.bottomMargin))
                        this.drawHorizontalLine(ix, ix+3, iy);
                  }
                  const iy = this.yPixel(yTicker.getX(i));
                  this.drawTextExponent('10', yTicker.getTickLabel(i),ix-5, iy, Graphix.MIDDLE | Graphix.RIGHT);
              } else {
                  if (yTicker.isVisible(i)) {
                    const iy = yTicker.getI(i) + this.topMargin;
                    this.drawText(yTicker.getTickLabel(i),ix-3, iy, Graphix.MIDDLE | Graphix.RIGHT);
                  }
              }
            }
        }
        // Draw vertical label
        this.context.save();
        this.context.textAlign = 'center';
        this.context.textBaseline = 'top';
        this.context.translate(0, this.topMargin + (this.height-this.leftMargin-this.rightMargin)/2);
        this.context.rotate(-Math.PI / 2);
        this.context.fillText(yLabel, 0, 0);
        this.context.restore();
    }
    

    plot(x,y, style = Graphix.SOLID, orientation = Graphix.HORIZONTAL, scaling = 1, xLimit = null, yShift = 0) {
        this.context.save();
        if (this.leftMargin+this.rightMargin+this.topMargin+this.bottomMargin>0)
            this.clipToAxes();
        if (xLimit === null)
            xLimit = [min(x), max(x)];
        if (x instanceof XArray)
            x = x.data;
        if (y instanceof XArray)
            y = y.data;
        if (style===Graphix.BAR) { // Bar graph
            if (orientation===Graphix.HORIZONTAL)
                this.plotBar(x,y,scaling);
            else
                this.plotBarVertical(x,y,scaling);
        } else if (style === Graphix.FILLED_COMPLEX) {
            var notyet = true;
            var iPixel = -1000000;
            for (var i=0; i<x.length; i++) {
                var ix = this.xPixel(x[i]);
                var phase = y.getValue(i).angle();
                if ((i>0)&&(ix>iPixel+1)) { // Make a loop
                    for (var ip = iPixel+1; ip<=ix; ip++) {
                        if ((ip>0)&&(ip<this.width)) {
                            var xx = this.xUser(ip);
                            var eta = (xx-x[i-1])/(x[i]-x[i-1]);
                            var rho1 = y.getValue(i-1).abs();
                            var rho2 = y.getValue(i).abs();
                            var phi1 = y.getValue(i-1).angle();
                            var phi2 = y.getValue(i).angle();
                            phi2 -= Math.floor((phi2-phi1)/2/Math.PI+.5)*2*Math.PI; // Unwrap
                            this.beginPath();
                            this.setStrokeStyle('rgb('+complexToRgb(1,(1-eta)*phi1+eta*phi2)+')');
                            this.moveTo(xx,0);
                            this.lineTo(xx,((1-eta)*rho1+eta*rho2)*scaling);
                            this.stroke();
                        }
                    }
                }
                else if ((ix>iPixel)&&(iPixel>0)&&(iPixel<this.width)) {
                    // console.log('Color = '+complexToRgb(1,phase));
                    this.beginPath();
                    this.setStrokeStyle('rgb('+complexToRgb(1,phase)+')');
                    this.moveTo(x[i],0);
                    this.lineTo(x[i],y.getValue(i).abs()*scaling);
                    this.stroke();
                }
                iPixel = ix;
            }
        } else if (style == Graphix.SYMBOLS) {
            for (var i=0; i<x.length; i++) {
                this.plotDisk(x[i], y[i], 7);
            }
        } else { // Standard plot
            this.beginPath();
            this.moveTo(x[0],(y[0]+yShift)*scaling);
            for (var i=1; i<x.length; i++) {
                this.lineTo(x[i],(y[i]+yShift)*scaling);
            }
            if (style===Graphix.FILLED) {
                this.lineTo(x[x.length-1],0);
                this.lineTo(x[0],0);
                this.context.fillStyle = this.context.strokeStyle;
                this.fill();
            }
            this.stroke();
        }
        this.context.restore();
    }

    fill() {
        this.context.fill();
    }
    
    stroke() {
        this.context.stroke();
    }
    
    // Convert user width to number of pixels (along horizontal axis)
    wPixel(width) {
        return Math.floor(.5+width/this.scaler[0]);
    }
    
    // Convert user to pixel coord (along horizontal axis)
    xPixel(x) {
        return this.leftMargin + Math.floor(.5+(x-this.scaler[1])/this.scaler[0]);
    }

    // Convert user to pixel coord (along vertical axis)
    yPixel(y) {
        return this.topMargin + Math.floor(.5+(y-this.scaler[3])/this.scaler[2]);
    }

    // Convert pixel to user coord (along horizontal axis)
    xUser(ix) {
        return (ix-this.leftMargin)*this.scaler[0] + this.scaler[1];
    }

    // Convert pixel to user coord (along vertical axis)
    yUser(iy) {
        return (iy-this.topMargin)*this.scaler[2]+this.scaler[3];
    }
    
    // Convert user point to pixel point
    pointPixel(p) {
        return new Point(this.xPixel(p.x), this.yPixel(p.y));
    }
    
    // Draw text at specified position (in pixels)
    drawText(text,x,y,alignment) {
        // Horizontal alignment
        var horizontalAlignment = alignment & 0x0C;
        switch(horizontalAlignment) {
            case Graphix.LEFT:
                this.context.textAlign = 'left';
                break;
            case Graphix.RIGHT:
                this.context.textAlign = 'right';
                break;
            case Graphix.CENTER:
                this.context.textAlign = 'center';
                break;
        }
        // Vertical Alignment
        var verticalAlignment = alignment & 0x03;
        switch(verticalAlignment) {
            case Graphix.TOP:
                this.context.textBaseline = 'top';
                break;
            case Graphix.MIDDLE:
                this.context.textBaseline = 'middle';
                break;
            case Graphix.BOTTOM:
                this.context.textBaseline = 'bottom';
                break;
        }
        this.context.fillStyle = this.context.strokeStyle;
        this.context.fillText(text,x,y);
    }
    
    drawTextExponent(text, exponent, x, y, alignment) {
        this.context.textBaseline = 'top';
        this.context.textAlign = 'right';
        const mainFontSize = this.fontSize;
        const expFontSize = Math.round(this.fontSize * 0.7)
        const textWidth = this.context.measureText(text).width;
        this.setFontSize(expFontSize);
        const exponentWidth = this.context.measureText(exponent).width;
        const totalWidth = textWidth + exponentWidth;
        const totalHeight = mainFontSize + expFontSize/3;
        this.context.fillText(exponent, x, y - totalHeight/2);
        this.setFontSize(mainFontSize);
        this.context.fillText(text, x - exponentWidth, y - totalHeight/2 + expFontSize/3);
    }
    
    // Draw text at specified position (in user coordinates)
    drawTextUser(text,x,y,alignment) {
        this.drawText(text, this.xPixel(x), this.yPixel(y), alignment);
    }
    drawImage(img, x, y, alignment = Graphix.CENTER | Graphix.MIDDLE) {
        if (img.complete) {
            var w = img.naturalWidth;
            var h = img.naturalHeight;
            var ix = this.xPixel(x);
            var iy = this.yPixel(y);
            // Horizontal alignment
            var horizontalAlignment = alignment & 0x0C;
            switch(horizontalAlignment) {
                case Graphix.LEFT:
                    break;
                case Graphix.RIGHT:
                    ix -= w;
                    break;
                case Graphix.CENTER:
                    ix -= w/2;
                    break;
            }
            // Vertical Alignment
            var verticalAlignment = alignment & 0x03;
            switch(verticalAlignment) {
                case Graphix.TOP:
                    break;
                case Graphix.MIDDLE:
                    iy -= h/2;
                    break;
                case Graphix.BOTTOM:
                    iy -= h;
                    break;
            }
            this.context.drawImage(img, ix, iy); 
        }
    }
    
    drawHorizontalLine(x1, x2, y) {
        this.beginPath();
        this.context.moveTo(x1, y);
        this.context.lineTo(x2, y);
        this.context.stroke();
    }
    
    drawVerticalLine(x, y1, y2) {
        this.beginPath();
        this.context.moveTo(x, y1);
        this.context.lineTo(x, y2);
        this.context.stroke();
    }

    drawThickHorizontalLine(x1,x2,y, nLines = 2) {
        for (var i =-nLines; i<=nLines; i++) {
            this.beginPath();
            this.context.moveTo(x1,y+i);
            this.context.lineTo(x2,y+i);
            this.context.stroke();
        }
    }

    drawThickVerticalLine(x,y1,y2) {
        for (var i =-2; i<=2; i++) {
            this.beginPath();
            this.context.moveTo(x+i,y1);
            this.context.lineTo(x+i,y2);
            this.context.stroke();
        }
    }
    
    drawHorizontalLineUser(x1, x2, y) {
        this.drawHorizontalLine(this.xPixel(x1), this.xPixel(x2), this.yPixel(y));
    }

    drawVerticalLineUser(x, y1, y2) {
        this.drawVerticalLine(this.xPixel(x), this.yPixel(y1), this.yPixel(y2));
    }
    
    drawThickVerticalLineUser(x, y1, y2) {
        this.drawThickVerticalLine(this.xPixel(x), this.yPixel(y1), this.yPixel(y2));
    }
    
    drawArrow(x1, y1, x2, y2, w, size) {
        this.context.fillStyle = this.context.strokeStyle;
        const angle = Math.atan2(y2 - y1, x2 - x1);
        const headSize = 15;
        this.context.beginPath();
        this.context.moveTo(x1, y1); 
        this.context.lineTo(x2, y2); 
        this.context.stroke(); 
        this.context.beginPath();
        this.context.moveTo(x2, y2); 
        this.context.lineTo(x2 - headSize * Math.cos(angle - Math.PI / 8),
                            y2 - headSize * Math.sin(angle - Math.PI / 8));
        this.context.lineTo(x2 - headSize * Math.cos(angle + Math.PI / 8),
                            y2 - headSize * Math.sin(angle + Math.PI / 8));
        this.context.closePath();
        this.context.fill();
    }
    
    drawArrowUser(x1, y1, x2, y2, w = 1, size = 1) {
        this.drawArrow(this.xPixel(x1), this.yPixel(y1), this.xPixel(x2), this.yPixel(y2), w, size);
    }

    drawHorizontalArrow(x, y, w, size) { 
        this.context.fillStyle = this.context.strokeStyle;
        var arrowX = x+w-size; 
        var arrowTopY = y - 0.6*size;  
        var arrowBottomY = y + 0.6*size; 
        this.context.beginPath();
        this.context.moveTo(x, y); 
        this.context.lineTo(x+w, y); 
        this.context.lineTo(arrowX, arrowTopY); 
        this.context.moveTo(x+w, y); 
        this.context.lineTo(arrowX, arrowBottomY); 
        this.context.stroke(); 
    } 
    
    drawHorizontalArrowUser(x, y, w, size) {
        var width = this.xPixel(x+w)-this.xPixel(x);
        this.drawHorizontalArrow(this.xPixel(x), this.yPixel(y), width, size);
    }

    drawVerticalArrow(x, y, h, size, doubleArrow = false) { 
        this.context.fillStyle = this.context.strokeStyle;
        var arrowY = y-h+size; 
        var arrowLeftX = x - 0.6*size;  
        var arrowRightX = x + 0.6*size; 
        this.context.beginPath();
        this.context.moveTo(x, y); 
        this.context.lineTo(x, y-h); 
        this.context.lineTo(arrowRightX, arrowY); 
        this.context.moveTo(x, y-h); 
        this.context.lineTo(arrowLeftX, arrowY); 
        if (doubleArrow) {
            this.context.moveTo(x,y);
            this.context.lineTo(arrowRightX, y-size);
            this.context.moveTo(x,y);
            this.context.lineTo(arrowLeftX, y-size);
        }
        this.context.stroke(); 
    } 
    
    drawVerticalArrowUser(x, y, h, size, doubleArrow = false) {
        var height = this.yPixel(y)-this.yPixel(y+h);
        this.drawVerticalArrow(this.xPixel(x), this.yPixel(y), height, size, doubleArrow);
    }
    
    // Draw traffic light
    plotTrafficLight(state) {
        color1 = 'lightgray';
        color2 = 'lightgray';
        color3 = 'lightgray';
        switch(state) {
            case Graphix.RED_LIGHT:
                color1 = 'red';
                break;
            case Graphix.YELLOW_LIGHT:
                color2 = 'orange';
                break;
            case Graphix.GREEN_LIGHT:
                color3 = 'green';
                break;
        }
        var wTrafficLight = 40;
        var margin = 10;
        this.beginPath();
        this.context.rect(width-hMargin/2-wTrafficLight,vMargin,2*wTrafficLight,100);
        this.setFillStyle('gray');
        this.fill();
        this.plotDiskPixel(width-hMargin/2,vMargin,wTrafficLight);
        this.plotDiskPixel(width-hMargin/2,vMargin+hMargin+6,wTrafficLight);
        this.setFillStyle(color1);
        this.plotDiskPixel(width-hMargin/2,vMargin,hMargin/4);
        this.setFillStyle(color2);
        this.plotDiskPixel(width-hMargin/2,vMargin+hMargin/2+3,hMargin/4);
        this.setFillStyle(color3);
        this.plotDiskPixel(width-hMargin/2,vMargin+hMargin+6,hMargin/4);
    }

    // Build a scaler array from specified value
    static buildScaler(width, height, x1, x2, y1, y2) {
        var result = Array(4);
        result[0] = (x2-x1)/width;
        result[1] = x1;
        result[2] = (y1-y2)/height;
        result[3] = y2;
        return result;
    }
    
    // ************** Hotspot management ***************
    
    clearHotspots() {
        this.hotspotArray = new Array();
    }
    
    // Add a point as new hotspot
    addHotspot(p, type = HOTSPOT_XY) {
        if (p instanceof Segment)
            type = HOTSEGMENT;
        if (type == HOTSEGMENT) { // Then p must be a Segment object
            this.hotspotArray.push(new Segment(this.pointPixel(p.a), this.pointPixel(p.b)));
        }
        else {
            this.hotspotArray.push(new Point(this.xPixel(p.x), this.yPixel(p.y)));
        }
        this.hotspotType.push(type);
    }
    
    // Update a specific hotspot
    updateHotspot(i, p) {
        if (i <= this.hotspotArray.length) {
            this.hotspotArray[i].x = this.xPixel(p.x);
            this.hotspotArray[i].y = this.yPixel(p.y);
        }
    }
        
    // Plot hotspot depending on status
    plotHotspots() {
        if (this.hotspotStatus!=0) {
            var p = this.hotspotArray[this.currentIndex];
            switch(this.hotspotType[this.currentIndex]) {
                case HOTLINE_Y:
                    this.setStrokeStyle('red');
                    this.drawThickHorizontalLine(0,2000,p.y, 1);
                    break;
                default:
                    this.setFillStyle('red');
                    this.plotDiskPixel(p.x,p.y,7);
            }
        }
    }
    
    hotspotHasMoved() {
        var result = this.hotspotMoved;
        this.hotspotMoved = false;
        return result;
    }

    hotspotStatusHasChanged() {   
        var result = this.hotspotStatusChanged;
        this.hotspotStatusChanged = false;
        return result;
    }
    
    // Compute distance from hotspot i to point p
    // taking into account hotspot type
    distanceBetween(i, p) {
        if (this.hotspotType[i]==HOTLINE_Y) // Horizontal line
            return Math.abs(p.y-this.hotspotArray[i].y);
        else if (this.hotspotType[i]==HOTLINE_X) // Vertical line
            return Math.abs(p.x-this.hotspotArray[i].x);
        else
            return this.hotspotArray[i].distanceTo(p);
    }
    
    // Process mouseMove event
    // p is mouse pointer position relative to top left corner
    mouseMove(p, buttonDown) {
        var previousHotspotStatus = this.hotspotStatus;
        if (buttonDown) { // Button down
            if (this.hotspotStatus==1)
                this.hotspotStatus = 2; // Hotspot clicked 
            if (this.hotspotStatus==2) { // process
                if (this.distanceBetween(this.currentIndex,p)>0) {
                    if ((this.hotspotType[this.currentIndex]==HOTSPOT_XY)||(this.hotspotType[this.currentIndex]==HOTSPOT_X)||(this.hotspotType[this.currentIndex]==HOTSPOT_X)) {
                        this.hotspotArray[this.currentIndex].x = p.x;
                        this.hotspotMoved = true;
                    }
                    if ((this.hotspotType[this.currentIndex]==HOTSPOT_XY)||(this.hotspotType[this.currentIndex]==HOTSPOT_Y)||(this.hotspotType[this.currentIndex]==HOTLINE_Y)) {
                        this.hotspotArray[this.currentIndex].y = p.y;
                        this.hotspotMoved = true;
                    }
                }
            }
        } else {
            var nearestIndex = -1;
            var distance = 1E9;
            for (var i=0; i<this.hotspotArray.length; i++) {
                if ((this.hotspotType[i]!=HOTSPOT_DISABLED)&&(this.distanceBetween(i,p)<distance)) {
                    nearestIndex = i;
                    distance = this.distanceBetween(i,p);
                }
            }
            if ((nearestIndex>=0)&&(distance<50)&&(!buttonDown)) {
                this.hotspotStatus = 1;
                this.currentIndex = nearestIndex;
            } else {
                this.hotspotStatus = 0;
                this.currentIndex = -1;
            }
        }
        if (this.hotspotStatus != previousHotspotStatus)
            this.hotspotStatusChanged = true;
    }
    
}


/*******************************************************************************
 * 
 * Graphics in 3D using THREE.js
 * 
 ******************************************************************************/
class ThreeEnvironment {
    constructor(canvas, upaxis = [0, 0, 1]) {
        this.canvas = canvas;
        this.renderer = new THREE.WebGLRenderer({ canvas: canvas, antialias: true });
        this.renderer.setSize(canvas.width, canvas.height); // canvas.style.width, canvas.style.height);
        this.renderer.setPixelRatio(window.devicePixelRatio);
        this.clock = new THREE.Clock();
        this.scene = new THREE.Scene();
        this.scene.background = new THREE.Color(0xFFFFFF);
        this.camera = new THREE.PerspectiveCamera(
          45,
          canvas.width / canvas.height,
          0.1,
          100
        );        
        this.camera.up.set(upaxis[0], upaxis[1], upaxis[2]);
        // Mouse-controlled scene rotation
        this.controls = new THREE.OrbitControls(this.camera, this.renderer.domElement);
        this.controls.enableDamping = true;
        // Lights
        this.scene.add(new THREE.AmbientLight(0xFFFFFF, .9));
        this.light = new THREE.DirectionalLight(0xFFFFFF,.8);
        // this.light.castShadow = true;
        this.light.position.set(10, 5, -5);
        this.scene.add(this.light);
         
        console.log('=====================================');
        console.log('THREE version '+THREE.REVISION+' initialized.');
        console.log('=====================================');
    }
    
    render() {
        this.renderer.render(this.scene, this.camera);
    }
}

class ThreeObject {
    setPosition(point) {
        this.object.position.set(point.x, point.y, point.z);
    }
    
    addTo(scene) {
        scene.add(this.object);
    }
}

// ==================================================
// Create a circle
// ==================================================
function createCircleOutline(radius = 1, segments = 64, color = 0x000000) {
  const points = [];
  for (let i = 0; i <= segments; i++) {
    const theta = (i / segments) * 2 * Math.PI;
    points.push(new THREE.Vector3(radius * Math.cos(theta),
                                  radius * Math.sin(theta),
                                  0));
  }
  const geometry = new THREE.BufferGeometry().setFromPoints(points);
  const material = new THREE.LineBasicMaterial({ color });
  return new THREE.LineLoop(geometry, material);
}
// ==================================================
// Segment of two THREE.Vector3
// ==================================================
class Segment3 {
  constructor(p0, p1) {
    this.p0 = p0.clone();
    this.p1 = p1.clone();
  }

  direction() {
    return this.p1.clone().sub(this.p0);
  }

  length() {
    return this.p0.distanceTo(this.p1);
  }

  midpoint() {
    return this.p0.clone().add(this.p1).multiplyScalar(0.5);
  }
  
}

// ==================================================
// Create a text as an item for THREE.js
// ==================================================
function makeTextSprite(text, {
  height = 0.5,          // ← WORLD units
  fontSize = 64,         // ← pixels
  fontFace = 'Arial',
  color = 'gray'
} = {}) {

  const canvas = document.createElement('canvas');
  const ctx = canvas.getContext('2d');

  ctx.font = `${fontSize}px ${fontFace}`;

  // Measure text
  const metrics = ctx.measureText(text);
  const textWidth = Math.ceil(metrics.width);
  const textHeight = fontSize; // good approximation

  // Canvas size
  canvas.width = textWidth;
  canvas.height = textHeight;

  // Redraw after resizing canvas
  ctx.font = `${fontSize}px ${fontFace}`;
  ctx.fillStyle = color;
  ctx.textAlign = 'left';
  ctx.textBaseline = 'top';
  ctx.fillText(text, 0, 0);

  const texture = new THREE.CanvasTexture(canvas);
  texture.needsUpdate = true;

  const material = new THREE.SpriteMaterial({
    map: texture,
    transparent: true
  });

  const sprite = new THREE.Sprite(material);

  // 🔑 Map pixel size → world size
  const aspect = textWidth / textHeight;
  sprite.scale.set(height * aspect, height, 1);

  return sprite;
}

// ==================================================
// Double-headed arrow (for crystal optical axis)
// The arrow is along the x axis
// ==================================================
function createDoubleArrow({length = 1.2, radius = 0.03,
                            headLength = 0, headRadius = 0, color = 0xff00ff,
                            direction = new THREE.Vector3(1, 0, 0)} = {}) {

  if (headLength == 0)
      headLength = 0.15*length;
  if (headRadius == 0)
      headRadius = 0.1*length;
  const arrow = new THREE.Group();
  const material = new THREE.MeshStandardMaterial({ color });

  // Shaft
  const shaftGeometry = new THREE.CylinderGeometry(
    radius,
    radius,
    length - 2 * headLength,
    16
  );
  // shaftGeometry.rotateZ(Math.PI/2); // So that arrow is aligned along x by default
  const shaft = new THREE.Mesh(shaftGeometry, material);
  arrow.add(shaft);

  // Arrowhead (top)
  const headGeom = new THREE.ConeGeometry(headRadius, headLength, 16);
  // headGeom.rotateZ(Math.PI/2);
  const head1 = new THREE.Mesh(headGeom, material);
  head1.rotation.z = Math.PI;
  head1.position.y = -(length - headLength) / 2;
  arrow.add(head1);

  // Arrowhead (bottom)
  const head2 = new THREE.Mesh(headGeom, material);
  head2.position.y = (length - headLength) / 2;
  arrow.add(head2);
  // Now align arrow from default y to specified direction
  arrow.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), direction);
  return arrow;
}

// ==================================================
// Double arrow
// ==================================================
class DoubleArrow{
    constructor(position, direction) {
        this.object = createDoubleArrow({length: 2.0, color: 0xFF0000, direction: direction});
        this.object.position.set(position.x, position.y, position.z);
    }
}

// ==================================================
// Create a polar arrow (double arrow inside a circle)
// ==================================================
class PolarArrow extends ThreeObject {
    constructor(position = [0, 0, 0]) {
        super();
        this.object = new THREE.Group();
        var arrow = createDoubleArrow({length: 2.0, color: 0xFF0000});
        this.object.add(arrow);
        const circle = createCircleOutline();
        this.object.add(circle);
        this.object.position.set(position[0], position[1], position[2]);
    }
    
    setAngle(angle) {
        const axis = new THREE.Vector3(0, 0, 1).normalize();
        this.object.quaternion.setFromAxisAngle(axis, angle);
    }
}

class SingleArrow extends ThreeObject{
    constructor(A, B, color = 0xFF0000, diameter = 0.05, brightness = 1) {
        super();
        const headLength = diameter*6;
        const headRadius = diameter*4;

        this.material = new THREE.MeshBasicMaterial({
            color:color,
            transparent: true, // Enable transparency
            opacity: brightness, // Adjust opacity to control visibility
        });

        const dir = new THREE.Vector3().subVectors(B, A);
        const length = dir.length();
        const geometry = new THREE.CylinderGeometry(diameter, diameter, length-headLength, 32, 1, true);
        const shaft = new THREE.Mesh(geometry, this.material);
        
        
        this.object = new THREE.Group();
        this.object.add(shaft);

        // Arrowhead
        const headGeom = new THREE.ConeGeometry(headRadius, headLength, 16);
        const head = new THREE.Mesh(headGeom, this.material);
        // head1.rotation.z = Math.PI;
        head.position.y = (length - headLength) / 2;
        this.object.add(head);

        this.object.position.copy(A).addScaledVector(dir, 0.5);
        this.object.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0),dir.clone().normalize());
    }
    
    setBrightness(value) {
        this.material.opacity = value;
    }
}


// ==================================================
// Beam texture for realistic laser beam (not operational)
// ==================================================
function makeBeamTexture(sizeU = 256, sizeV = 64) {
  const canvas = document.createElement('canvas');
  canvas.width = sizeU;
  canvas.height = sizeV;
  const ctx = canvas.getContext('2d');

  for (let x = 0; x < sizeU; x++) {
    for (let y = 0; y < sizeV; y++) {
      const r = (y - sizeV/2) / (sizeV/2);
      const radial = Math.exp(- r * r);
      const axial = 1; // 0.8 + 0.2 * Math.cos(Math.PI * x / sizeU);

      const alpha = 1; // radial * axial;
      ctx.fillStyle = `rgba(255,0,0,${alpha})`;
      ctx.fillRect(x, y, 1, 1);
    }
  }
  return new THREE.CanvasTexture(canvas);
}

// ==================================================
// Radial gradient (not operational)
// ==================================================
function makeRadialGradientTexture(size = 256) {
  const canvas = document.createElement('canvas');
  canvas.width = canvas.height = size;
  const ctx = canvas.getContext('2d');

  const g = ctx.createRadialGradient(
    size/2, size/2, 0,
    size/2, size/2, size/2
  );

  g.addColorStop(0, 'rgba(255,0,0,1)');
  g.addColorStop(0.3, 'rgba(255,0,0,0.6)');
  g.addColorStop(1, 'rgba(255,0,0,0)');

  ctx.fillStyle = g;
  ctx.fillRect(0, 0, size, size);

  return new THREE.CanvasTexture(canvas);
}


class LightBeam extends ThreeObject{
    constructor(A, B, color = 0xFF0000, brightness = 1) {
        super();
        this.material = new THREE.MeshBasicMaterial({
            color:0xFF0000,
            transparent: true, // Enable transparency
            opacity: brightness, // Adjust opacity to control visibility
            depthTest: true,
            depthWrite: false,
            // depthWrite: false, // Disable depth writing (allows objects behind to be visible)
            // blending: THREE.AdditiveBlending, // Use additive blending for glow effect
        });

        const beamDiameter = 0.05;
        const dir = new THREE.Vector3().subVectors(B, A);
        const length = dir.length();
        const geometry = new THREE.CylinderGeometry(beamDiameter, beamDiameter, length, 32, 1, true);
        this.object = new THREE.Mesh(geometry, this.material);
        this.object.position.copy(A).addScaledVector(dir, 0.5); // 0.5);
        this.object.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0),dir.clone().normalize());
    }
    
    setBrightness(value) {
        this.material.opacity = value;
    }
}

// ==================================================
// Detector group (hemisphere + sensor)
// ==================================================
class PhotoDetector extends ThreeObject {
    constructor(label = '', radius = 0.4, radiusSensor = 0.2) {
        super();
        this.radius = radius;
        this.radiusSensor = radiusSensor;
        this.label = label;
        this.object = this.buildDetector(radius, radiusSensor);
        this.flashTime = 0; // Remaining time with detector flash ON
        this.flashDuration = 0.2; // Default flash duration
        this.sensorIntensityOff = 0.5;
        this.sensorIntensityOn = 3;
    }
        
    buildDetector(radius, radiusSensor) {
        const detector = new THREE.Group();
        // ---- Hemisphere (mirror) ----
        const hemiGeometry = new THREE.SphereGeometry(radius, 16, 8, 0,
                                                      Math.PI * 2, 0, Math.PI/2);
        hemiGeometry.rotateX(Math.PI / 2);

        const hemiMaterial = new THREE.MeshStandardMaterial({
          color: 0xC0C0C0,
          roughness: 0.7,
          transparent: true,
          opacity: 0.35,
          side: THREE.DoubleSide
        });

        const hemisphere = new THREE.Mesh(hemiGeometry, hemiMaterial);
        detector.add(hemisphere);

        // Add wireframe
        const edges = new THREE.EdgesGeometry(hemiGeometry);
        const wireframe = new THREE.LineSegments(edges,
                                   new THREE.LineBasicMaterial({ color: 0x808080 }));
        detector.add(wireframe);

        // ---- Sensor (small sphere at focal point) ----
        const sensorGeometry = new THREE.SphereGeometry(radius * radiusSensor, 16, 16);
        const sensorMaterial = new THREE.MeshStandardMaterial({
          color: 0x000000,
          emissive: 0x550000
        });

        const sensor = new THREE.Mesh(sensorGeometry, sensorMaterial);
        sensor.emissiveIntensity = this.sensorIntensityOff;
        sensor.position.set(0, 0, radius/2);
        detector.add(sensor);

        // Create double arrow to show associated polarization
        /*
        if (this.arrowDirection != null) {
          let arrow = createDoubleArrow({length: 1.0, color: 0xFF0000});
          arrow.position.set(0, 0, 1.3*radius);
          // const quaternion = new THREE.Quaternion();
          console.log('rotation axis '+this.beamAxis.x+' '+this.beamAxis.y+' '+this.beamAxis.z);
          
          arrow.quaternion.setFromAxisAngle(this.beamAxis, this.arrowAngle);
          
          
          // quaternion.setFromAxisAngle(this.beamAxis, this.arrowAngle);
          // arrow.quaternion.multiply(quaternion);          
//          arrow.rotateZ(this.arrowAngle);
          detector.add(arrow);
        }
        */

        if (this.label.length>0) {
          const text = makeTextSprite(this.label);
          text.position.set(0, 1.5*radius, 0);
          detector.add(text);
        }

        // Keep reference of sensor and sensorMaterial for animation when photon is absorbed
        this.sensor = sensor;
        this.sensorMaterial = sensorMaterial;

        return detector;
      }
      
    placeDetectorAlongBeam(beam, distance = 0) {

      const direction = beam.direction().normalize();

      // Position detector along the beam
      const position = beam.p1.clone().addScaledVector(direction, distance);
      this.object.position.copy(position);

      // Orient hemisphere toward incoming beam
      const target = position.clone().add(direction);
      this.object.lookAt(target);
    }

    updateSensorFlash(dt) {
      if (this.flashTime>0) { // Then flash the sensor
          this.sensor.scale.setScalar(1+2*this.flashTime/this.flashDuration);
          this.flashTime = this.flashTime - dt;
          if (this.flashTime == 0)
              this.flashTime = -1; // To ensure the flash is erased at next call
          const mat = this.sensorMaterial;
          mat.emissiveIntensity = this.sensorIntensityOn;
      } else if (this.flashTime<0) { // Then erase the flash
          this.sensor.scale.setScalar(1);
          this.flashTime = 0;
          const mat = this.sensorMaterial;
          mat.emissiveIntensity = this.sensorIntensityOff;
      }
    }
    
    flash() {
        this.flashTime = this.flashDuration; // Set flash duration to 0.2 s.
    }
}

// ==================================================
// Create a triangular prism extruded along X
// ==================================================
function createPrism(pointsXY, length, material) {

  const shape = new THREE.Shape();
  shape.moveTo(pointsXY[0].x, pointsXY[0].y);
  for (let i = 1; i < pointsXY.length; i++) {
    shape.lineTo(pointsXY[i].x, pointsXY[i].y);
  }
  shape.closePath();

  const geometry = new THREE.ExtrudeGeometry(shape, {
    depth: length,
    bevelEnabled: false
  });

  // Extrusion is along +Z → rotate so it is along +Y
  geometry.rotateX(Math.PI / 2);
  geometry.translate(0, 0, 0);

  return new THREE.Mesh(geometry, material);
}

// ==================================================
// Prism group (rotates around beam axis)
// ==================================================
function createPrismGroup(beamAxis, vertAxis, width = 1) {
    const perpAxis = beamAxis.clone().cross(vertAxis);
    console.log('beamAxis '+beamAxis.x+' '+beamAxis.y+' '+beamAxis.z);
    console.log('vertAxis '+vertAxis.x+' '+vertAxis.y+' '+vertAxis.z);
    console.log('perpAxis '+perpAxis.x+' '+perpAxis.y+' '+perpAxis.z);
    const L = 2.0;     // length along beam (x)
    const h = 1.0;     // prism height
    const gap = 0.02;  // small separation → visible interface

    const prismGroup = new THREE.Group();

    const prismMaterial = new THREE.MeshPhysicalMaterial({
      color: 0xCCCCCC,
      transparent: true,
      opacity: 0.3,
      roughness: 0.6,
      // transmission: 0.9,
      // thickness: 1.0
    });

    const diopterMaterial = new THREE.MeshBasicMaterial({
        color: 0xA0A0A0,
        // transparent: true,
        opacity: 0.9,
    });
    /*
      color: 0xFF0000,
    });*/

/*
    // Upper prism
    const prism1 = createPrism(
      [
        { x: -w, y: w },
        { x:  w, y: w },
        { x:  w, y:  -w }
      ],
      L,
      prismMaterial
    );
    prism1.position.y = w;
    prism1.position.z = gap;
    // prismGroup.add(prism1);

    // Lower prism
    const prism2 = createPrism(
      [
        { x: -w, y: w },
        { x:  -w, y: -w },
        { x:  w, y: -w }
      ],
      L,
      prismMaterial
    );
    prism2.position.z = -gap;
    prism2.position.y = w;
    // prismGroup.add(prism2);
*/
    
    // Build cube in local frame
    const cubeGeometry = new THREE.Geometry();
    cubeGeometry.vertices = [
        new THREE.Vector3(-1, -1, -1),
        new THREE.Vector3(-1, 1, -1),
        new THREE.Vector3(-1, 1, 1),
        new THREE.Vector3(-1, -1, 1),
        new THREE.Vector3(1, -1, -1),
        new THREE.Vector3(1, 1, -1),
        new THREE.Vector3(1, 1, 1),
        new THREE.Vector3(1, -1, 1)
    ];
    cubeGeometry.faces = [
        new THREE.Face3(0, 2, 1), // Bootom
        new THREE.Face3(0, 3, 2),
        new THREE.Face3(4, 5, 6), // Top
        new THREE.Face3(4, 6, 7),
        new THREE.Face3(0, 1, 5), // Back
        new THREE.Face3(0, 5, 4),
        new THREE.Face3(2, 3, 6), // Front 
        new THREE.Face3(3, 7, 6),
        new THREE.Face3(1, 6, 5), // Right
        new THREE.Face3(1, 2, 6),
        new THREE.Face3(0, 4, 7), // Left
        new THREE.Face3(0, 7, 3),
    ];
    // Now compute geometry properly aligned
    const geometry = cubeGeometry.clone(); 
    for (let i = 0; i<geometry.vertices.length; i++) {
        geometry.vertices[i] = beamAxis.clone().multiplyScalar(cubeGeometry.vertices[i].z*width/2);
        geometry.vertices[i].addScaledVector(vertAxis, cubeGeometry.vertices[i].x*width/2);
        geometry.vertices[i].addScaledVector(perpAxis, cubeGeometry.vertices[i].y*width/2);
    }
    geometry.computeVertexNormals(); // For lighting
    const mesh = new THREE.Mesh(geometry, prismMaterial);
    const diopterGeometry = geometry.clone();
    diopterGeometry.faces = [
        new THREE.Face3(2, 3, 5),
        new THREE.Face3(3, 4, 5),
        new THREE.Face3(2, 5, 3),
        new THREE.Face3(3, 5, 4),
    ]
    diopterGeometry.computeVertexNormals(); // For lighting
    prismGroup.add(new THREE.Mesh(diopterGeometry, diopterMaterial));
    prismGroup.add(mesh);

    // Optical axis indicator (double arrows)
    const opticalAxisArrow = createDoubleArrow({length: 0.6*width, color: 0xFFFFFF, direction: beamAxis});
    let pos = beamAxis.clone().multiplyScalar(-.15*width);
    pos.addScaledVector(vertAxis, -.45*width);
    pos.addScaledVector(perpAxis, 0.45*width);
    opticalAxisArrow.position.set(pos.x, pos.y, pos.z);
    // opticalAxisArrow.position.set(-0.9, 0.9, -0.3); // Place it on the side of the prism
    prismGroup.add(opticalAxisArrow); // IMPORTANT: attach to prism group so it rotates correctly
    const opticalAxisArrow2 = createDoubleArrow({length: 0.6*width, color: 0xFFFFFF, direction: perpAxis});
    pos = beamAxis.clone().multiplyScalar(.45*width);
    pos.addScaledVector(vertAxis, .45*width);
    opticalAxisArrow2.position.set(pos.x, pos.y, pos.z);
    // opticalAxisArrow2.position.set(0.9, 0, 0.9); // Place it on the side of the prism
    prismGroup.add(opticalAxisArrow2); // IMPORTANT: attach to prism group so it rotates correctly
    
    return prismGroup;
}

// ==================================================
// Draw ordinary and extraordinary beam in Rochon prism
// Return a Segment3 corresponding to extraordinary beam
// ==================================================
function generateAllBeams(prismGroup, beamAxis, vertAxis, width) {
  const color = 0xff0000;
  // Incident beam
  const center = new THREE.Vector3();
  // const A = beamAxis.clone().multiplyScalar(-10);
  const B = beamAxis.clone().multiplyScalar(4);
  // prismGroup.add((new LightBeam(A, center, color)).object);
  // Output ordinary beam
  prismGroup.mainLightBeam = new LightBeam(center, B, color);
  prismGroup.add(prismGroup.mainLightBeam.object);
  // Compute geometry of refraced extraordinary beam
  const no = 1.6;
  const ne = 1.9;
  const innerAngle = Math.PI/4-Math.asin(no/ne/Math.sqrt(2));
  const cubeWidth = width;
  const shift = cubeWidth/2*Math.tan(innerAngle);
  const C = new THREE.Vector3();
  C.addScaledVector(beamAxis, cubeWidth/2);
  C.addScaledVector(vertAxis, shift);
  prismGroup.deflectedInnerLightBeam = new LightBeam(center, C, color);
  prismGroup.deflectedInnerLightBeam.addTo(prismGroup);
  const outerAngle = Math.asin(ne*Math.sin(innerAngle));
  const L = 4 - cubeWidth/2;
  const D = C.clone();
  D.addScaledVector(beamAxis, L*Math.cos(outerAngle)).addScaledVector(vertAxis, L*Math.sin(outerAngle));
  // const p0 = new THREE.Vector3(shift, 0, cubeWidth/2);
  // const p1 = new THREE.Vector3(shift+L*Math.sin(outerAngle), 0, cubeWidth/2+L*Math.cos(outerAngle));
  prismGroup.deflectedLightBeam = new LightBeam(C, D, color);
  prismGroup.deflectedLightBeam.addTo(prismGroup);
  return new Segment3(C,D);
}

class RochonPrism extends ThreeObject {
    constructor(beamAxis = new THREE.Vector3(0, 0, 1), vertAxis = new THREE.Vector3(1, 0, 0), width = 1) {
        super();
        this.axis = beamAxis;
        this.vertAxis = vertAxis;
        this.object = createPrismGroup(beamAxis, vertAxis, width);
        this.extraBeam = generateAllBeams(this.object, beamAxis, vertAxis, width);
        this.ordinaryBeam = new Segment3(new THREE.Vector3(0, 0, 0), beamAxis.clone().multiplyScalar(4));
    }
    
    setAngle(angle) {
        this.object.quaternion.setFromAxisAngle(this.axis, angle);        
    }
    
    createDetectors() {
        this.detector1 = new PhotoDetector();
        const horAxis = this.axis.clone().cross(this.vertAxis);
        this.detector2 = new PhotoDetector();
        // Ordinary beam detector
        this.detector1.placeDetectorAlongBeam(this.ordinaryBeam);
        // Extraordinary beam detector
        this.detector2.placeDetectorAlongBeam(this.extraBeam);
        this.object.add(this.detector1.object);
        this.object.add(this.detector2.object);

        const vertArrow = createDoubleArrow({length: 1.0, color: 0xFF0000, direction:this.vertAxis});
        const pos1 = this.ordinaryBeam.p1.clone().addScaledVector(this.ordinaryBeam.direction().normalize(), 1.3*this.detector1.radius);
        vertArrow.position.set(pos1.x, pos1.y, pos1.z);
        this.object.add(vertArrow);
        const horArrow = createDoubleArrow({length: 1.0, color: 0xFF0000, direction:horAxis});
        const pos2 = this.extraBeam.p1.clone().addScaledVector(this.extraBeam.direction().normalize(), 1.3*this.detector2.radius);
        horArrow.position.set(pos2.x, pos2.y, pos2.z);
        this.object.add(horArrow);
        
        // Draw detector labels
        const perpAxis = this.axis.clone().cross(this.vertAxis);
        const posA = this.ordinaryBeam.p1.clone().addScaledVector(perpAxis, 1.5*this.detector1.radius);
        const textA = makeTextSprite('A');
        textA.position.set(posA.x, posA.y, posA.z);
        this.object.add(textA);
        const posB = this.extraBeam.p1.clone().addScaledVector(perpAxis, 1.5*this.detector1.radius);
        const textB = makeTextSprite('B');
        textB.position.set(posB.x, posB.y, posB.z);
        this.object.add(textB);

        
        console.log('horAxis : '+horAxis.x+' '+horAxis.y+' '+horAxis.z);
    }
    
    setBeamBrightness(a, b) {
        this.object.mainLightBeam.setBrightness(a);
        this.object.deflectedInnerLightBeam.setBrightness(b);
        this.object.deflectedLightBeam.setBrightness(b);
    }

}

class PBS extends ThreeObject {
    constructor(inputAxis = new THREE.Vector3(0, 0, 1), outputAxis = new THREE.Vector3(0, 1, 0)) {
        super();
        this.cubeWidth = 1.5;
        // Build vertical axis
        const vertAxis = outputAxis.clone().cross(inputAxis);
        // Build first prism in local frame
        const prismGeom1 = new THREE.Geometry();
        prismGeom1.vertices = [
            new THREE.Vector3(-1, -1, -1),
            new THREE.Vector3(-1, 1, -1),
            new THREE.Vector3(-1, 1, 1),
            new THREE.Vector3(1, -1, -1),
            new THREE.Vector3(1, 1, -1),
            new THREE.Vector3(1, 1, 1),
        ];
        prismGeom1.faces = [
            new THREE.Face3(0, 2, 1), // Bottom
            new THREE.Face3(3, 4, 5), // Top
            new THREE.Face3(0, 1, 4), // Back
            new THREE.Face3(0, 4, 3),
            new THREE.Face3(1, 5, 4), // Right
            new THREE.Face3(1, 2, 5),
            new THREE.Face3(0, 5, 2), // Inside
            new THREE.Face3(0, 3, 5),
        ];
        // Build second prism in local frame
        const prismGeom2 = new THREE.Geometry();
        prismGeom2.vertices = [
            new THREE.Vector3(-1, -1, -1),
            new THREE.Vector3(-1, 1, 1),
            new THREE.Vector3(-1, -1, 1),
            new THREE.Vector3(1, -1, -1),
            new THREE.Vector3(1, 1, 1),
            new THREE.Vector3(1, -1, 1)
        ];
        prismGeom2.faces = [
            new THREE.Face3(0, 2, 1), // Bottom
            new THREE.Face3(3, 4, 5), // Top
            new THREE.Face3(0, 4, 3), // Inside
            new THREE.Face3(0, 1, 4),
            new THREE.Face3(1, 2, 5), // Front 
            new THREE.Face3(1, 5, 4),
            new THREE.Face3(0, 3, 5), // Left
            new THREE.Face3(0, 5, 2),
        ];
        const insideGeom = new THREE.Geometry();
        insideGeom.vertices = [
            new THREE.Vector3(-1, -1, -1),
            new THREE.Vector3(-1, 1, 1),
            new THREE.Vector3(1, -1, -1),
            new THREE.Vector3(1, 1, 1),
        ];
        insideGeom.faces = [
          new THREE.Face3(0, 1, 2),
          new THREE.Face3(1, 3, 2),
          new THREE.Face3(0, 2, 1),
          new THREE.Face3(1, 2, 3),
        ];
       
        this.object = new THREE.Group();
        const prismMaterial = new THREE.MeshPhysicalMaterial({
          color: 0x00CCCC,
          transparent: true,
          opacity: 0.3,
          roughness: 0.6,
          // transmission: 0.9,
          // thickness: 1.0
        });
        const prismMaterial2 = new THREE.MeshPhysicalMaterial({
          color: 0x00FF80,
          transparent: true,
          opacity: 0.3,
          roughness: 0.6,
          // transmission: 0.9,
          // thickness: 1.0
        });
        const insideMaterial = new THREE.MeshPhysicalMaterial({
          color: 0xA0A0A0,
          emissive: 0xFF0000,
          transparent: true,
          opacity: 0.3,
          roughness: 0.1,
          // transmission: 0.2,
          // thickness: 1.0
        });
        
        this.object.add(new THREE.Mesh(this.alignGeom(prismGeom1, inputAxis, outputAxis, vertAxis), prismMaterial));
        this.object.add(new THREE.Mesh(this.alignGeom(prismGeom2, inputAxis, outputAxis, vertAxis), prismMaterial2));
        // this.object.add(new THREE.Mesh(this.alignGeom(insideGeom, inputAxis, outputAxis, vertAxis), insideMaterial));
    }
    
    alignGeom(geom, inputAxis, outputAxis, vertAxis) {
        const result = geom.clone();
        for (let i = 0; i<result.vertices.length; i++) {
            result.vertices[i] = inputAxis.clone().multiplyScalar(geom.vertices[i].z*this.cubeWidth/2);
            result.vertices[i].addScaledVector(vertAxis, geom.vertices[i].x*this.cubeWidth/2);
            result.vertices[i].addScaledVector(outputAxis, geom.vertices[i].y*this.cubeWidth/2);
        }
        result.computeVertexNormals(); // For lighting
        return result;
    }
}

// ==================================================
// Mirror normal to the specified axis
// ==================================================
class Mirror extends ThreeObject {
    constructor(axis, radius = 0.5, thickness = 0.2, bs = false) {
        super();
        let color = 0x00FFFF;
        if (bs)
            color = 0xFFFF00;
        // Define materials
        const mirrorMaterial = new THREE.MeshStandardMaterial({
          color: color,
          metalness: 0.5,
          roughness: 0.5,
          transparent: bs,
        });
        const sideMaterial = new THREE.MeshStandardMaterial({
          color: 0x808080,
          metalness: 0.0,
          roughness: 0.7, // 0.7
          transparent: bs,
        });      
        // Define geometry
        const geometry = new THREE.CylinderGeometry(
          radius,          // top radius
          radius,          // bottom radius
          thickness,       // height (thickness of the mirror)
          64,              // radial segments
          8,               // height segments
          false            // open-ended
        );
        // Assign materials to specific faces
        const materials = [
          sideMaterial,    // side
          mirrorMaterial,  // top (mirror surface)
          sideMaterial,    // bottom
        ];
        
        // Create object
        this.object = new THREE.Group();
        const mirror = new THREE.Mesh(geometry, materials);     
        mirror.position.set(0, -thickness/2, 0);
        this.object.add(mirror);
        // Align mirror
        this.object.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), axis);
        // this.object.geometry.computeVertexNormals(); // For lighting

    }

}

class BeamSplitter extends Mirror {
    constructor(axis, radius = 0.5) {
        super(axis, radius, 0.05, true);
    }
    
}


// ==================================================
// Knob that can be rotated using setAngle function
// ==================================================
class Knob extends ThreeObject {
    constructor(axis) {
        super();
        
        this.object = new THREE.Group();
        
        // Materials
        this.metal = new THREE.MeshStandardMaterial({
          color: 0xA0A080,
          metalness: 0.4,
          roughness: 0.35
        });

        this.markMaterial = new THREE.MeshStandardMaterial({
          color: 0x222222,
          roughness: 0.8,
          metalness: 0.0, // 0.2
          emissive: 0x101010,     // key line
          emissiveIntensity: 1.0
        });
        
        this.shaftLength = 1.0;
        /* Knob body */
        this.radius = 0.3;
        const thickness = 0.4;
        const body = new THREE.Mesh(
          new THREE.CylinderGeometry(this.radius, this.radius, thickness, 128),
          this.metal
        );
        body.rotation.x = Math.PI / 2;
        body.position.set(0, 0, this.shaftLength+thickness/2);
        this.object.add(body);

        /* ---------------- Graduations ---------------- */
        const majorDiv = 10;      // e.g. 10 turns per revolution
        const minorDiv = 50;      // Thorlabs-like
        for (let i = 0; i < minorDiv; i++) {
          const angle = i * 2 * Math.PI / minorDiv;

          if (i % (minorDiv / majorDiv) === 0) {
            this.addTick(angle, 0.25, 0.03); // major tick
          } else {
            this.addTick(angle, 0.15, 0.02); // minor tick
          }
        }

        /* ---------------- Shaft ---------------- */
        const shaft = new THREE.Mesh(
          new THREE.CylinderGeometry(0.2, 0.2, this.shaftLength, 32),
          this.metal
        );
        shaft.rotation.x = Math.PI / 2;
        shaft.position.z = this.shaftLength/2;
        this.object.add(shaft);
        this.object.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), axis);
        this.baseQuaternion = this.object.quaternion.clone();
    }
    
    addTick(angle, height, thickness) {
        const tick = new THREE.Mesh(
          new THREE.BoxGeometry(thickness, 0.02, height),
          this.markMaterial
        );

        tick.position.set(
          Math.cos(angle) * (this.radius + thickness / 2),
          Math.sin(angle) * (this.radius + thickness / 2),
          height/2+this.shaftLength
        );

        tick.rotation.z = angle;
        this.object.add(tick);
    }
    
    setAngle(angle) {
        const qRot = new THREE.Quaternion();
        qRot.setFromAxisAngle(new THREE.Vector3(0, 0, 1), -angle);
        this.object.quaternion.copy(this.baseQuaternion).multiply(qRot);
    }

}



/*******************************************************************************
 * 
 * I/O functions
 * 
 ******************************************************************************/

// Save canvas in png file using provided filename
function saveImage(filename) {
    var data = canvas.toDataURL('image/png',1.0);
    var a = document.createElement('a');
    a.href = data;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
}

// Save spacified canvas in png file using provided filename
function saveImage(canvas, filename) {
    console.log('Saving image');
    var data = canvas.toDataURL('image/png',1.0);
    var a = document.createElement('a');
    a.href = data;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
}
/*******************************************************************************
 * 
 * Http communications
 * 
 ******************************************************************************/

function createHttpRequestObject() {
    var ro;
    var browser = navigator.appName;
    if(browser === "Microsoft Internet Explorer"){
        ro = new ActiveXObject("Microsoft.XMLHTTP");
    }else{
        ro = new XMLHttpRequest();
    }
    return ro;
}

// Determine language
const urlParams = new URLSearchParams(window.location.search);
const hasLang = urlParams.has('lang');
var lang = 0;
if (hasLang) {
    lang = parseInt(urlParams.get('lang'));
}

// Return French or English string depending on value of lang
function getFE(frenchString, englishString) {
    if (lang === 1)
        return englishString;
    else
        return frenchString;
}