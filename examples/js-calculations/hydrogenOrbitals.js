// (c) 2020 Manuel Joffre
// Atomic orbitals of the Hydrogen atom
/*******************************************************************************
 * 
 * www.quantum-physics.polytechnique.fr
 * 
 *
 ******************************************************************************/

/*
 * 
 * The radial variable r varies between 0 and 6n², which seems to encompass most of the wavefunction.
 * 
 * 
 * 
 * 
 * 
 * 
 * 
 */

var selectN = document.querySelector('#selectNId');
var selectEll = document.querySelector('#selectEllId');
var selectM = document.querySelector('#selectMId');
var selectPlotType = document.querySelector('#selectPlotTypeId');
var selectMeridian = document.querySelector('#selectMeridianId');
var selectParallel = document.querySelector('#selectParallelId');
var ecorcheCheckbox = document.querySelector('#checkEcorcheId');
var autoRotationCheckbox = document.querySelector('#checkAutoRotationId');
// var timeEvolutionCheckbox = document.querySelector('#checkTimeEvolutionId');
var probaRange = document.querySelector('#rangeProbaId');
var paramProba = document.querySelector('#paramId1');

var parameters = 
{
        n: 7, // 7
        ell: 4, // 4
        m: 2, // 2
        proba: 0.5,
        type: 'Complex surface',
        nTheta: 50,
        nPhi: 500, // 50
        ecorche: true,
        autorotate: false,
        timeEvolution: false,
};

var autosave = false; // Switch to true to save png file
var autosaveIndex = 4; // Type of autosave

var zoomFactor = .02;
var currentTime = 0;
var timeOrigin = 0;

if (autosave) {
    switch(autosaveIndex){
        case 1: // Family of orbitals
            zoomFactor = 0.12;
            break;
        case 2: // Rotating orbital
            zoomFactor = 0.015;
            parameters.n = 9;
            parameters.ell = 7;
            parameters.m = 3;
            parameters.ecorche = false;
            parameters.autorotate = true;
            break;
        case 3: // Variation of P
            zoomFactor = 0.025;
            parameters.n = 6;
            parameters.ell = 4;
            parameters.m = 2;
            parameters.ecorche = true;
            parameters.autorotate = false;
            parameters.proba = 0.1;
        case 4: // Gallery
            zoomFactor = 1.0/25; // /25.; // 1 for scaled, 1/25. for n=5 with fixed scale // 0.065 Pour n=4
            parameters.n = 1;
            parameters.ell = 0;
            parameters.m = 0;
            parameters.ecorche = false;
            parameters.autorotate = false;
            parameters.proba = 0.35;
    }
}

var computationIndex = 0;

var animationIndex = 0;

var canvas;

function onDocumentKeyDown(event) {
    var nSteps = 11;
    var keyCode = event.which;
    var backward = false;
    console.log(keyCode);
    if (keyCode==34)
        animationStep = (animationStep+1)%nSteps;
    else if (keyCode==33) {
        animationStep--;
        if (animationStep<0)
            animationStep = nSteps-1;
        backward = true;
    } else
        return;
    var initRequired = true;
    console.log(animationStep);
    switch(animationStep) {
        case 0:
            gui.close();
            parameters.screenVisible = false;
            parameters.waveVisible = true;
            parameters.showLeft = true;
            parameters.showRight = false;
            break;
        case 1:
            parameters.screenVisible = false;
            parameters.waveVisible = true;
            parameters.showLeft = true;
            parameters.showRight = true;
            break;
        case 2:
            parameters.screenVisible = true;
            parameters.showLeft = true;
            parameters.showRight = true;
            parameters.nPhotonsLog = 3;
            break;
        case 3:
            parameters.screenVisible = true;
            parameters.showLeft = true;
            parameters.showRight = true;
            parameters.waveVisible = false;
            break;
        case 4: // Start wavelength animation
            animationIndex = 0;
            break;
        case 5: // Start slit distance animation
            parameters.wavelength = 0.6;
            animationIndex = 0;
            break;
        case 6:
            parameters.slitSeparation = 600;
            parameters.wavelength = 0.6;
            parameters.screenVisible = true;
            parameters.showLeft = true;
            parameters.showRight = false;
            parameters.waveVisible = false;
            break;
        case 7:
            parameters.screenVisible = true;
            parameters.showLeft = true;
            parameters.showRight = false;
            parameters.waveVisible = false;
            parameters.nPhotonsLog = -1;
            break;
        case 8:
            parameters.screenVisible = true;
            parameters.showLeft = true;
            parameters.showRight = true;
            parameters.waveVisible = false;
            parameters.nPhotonsLog = -1;
            break;
        case 9:
            parameters.screenVisible = true;
            parameters.showLeft = true;
            parameters.showRight = true;
            parameters.waveVisible = false;
            parameters.nPhotonsLog = 0;
            if (!backward)
                initRequired = false;
            break;
        case 10:
            parameters.screenVisible = true;
            parameters.showLeft = true;
            parameters.showRight = true;
            parameters.waveVisible = false;
            parameters.nPhotonsLog = 1;
            if (!backward)
                initRequired = false;
            break;
    }
    if (initRequired)
        initAll();
}

var animationStep = -1; // For sequential animations

var nEtaTable = 100;

/**
 * 
 * Table relating probability value to wavefunction value (called eta)
 */
var lookupEtaTable = Array(nEtaTable);

// Compute lookupEtaTable
function initEtaTable(n, ell) {
    // Compute max value of wavefunction magnitude |psi|
    var maxValue = maxAbs(lookupTableRadial)*maxAbs(lookupTableLegendre);
    // Histogram array initialization
    var nHisto = nEtaTable;
    var histogram = Array(nHisto);
    histogram.fill(0);
    // Compute an histogram representing the contribution to the total probability
    // as a function of |psi|
    for (var iR=0; iR<lookupTableRadial.length; iR++) { // Loop over radial coordinate r
        var r = iR*deltaR;
        for (var iTheta = 0; iTheta<lookupTableLegendre.length; iTheta++) { // Loop over theta
            var theta = iTheta*deltaTheta;
            // Compute wavefunction from lookup tables
            var psi = lookupTableRadial[iR]*lookupTableLegendre[iTheta];
            var iHisto = Math.floor(Math.abs(psi)/maxValue*nHisto);
            iHisto = Math.min(iHisto,nHisto-1);
            histogram[iHisto] += Math.abs(sqr(psi))*r*r*deltaR*Math.sin(theta)*deltaTheta;
        }
    }
    // Compute cumulative sum to determine probability as a function of |psi|
    var totalCount = sum(histogram);
    lookupEtaTable[0] = histogram[0]/totalCount;
    for (var i=1; i<nEtaTable; i++) {
        lookupEtaTable[i] = lookupEtaTable[i-1] + histogram[i]/totalCount;
    }
    lookupEtaTable[nEtaTable-1] = 1; // More accurate
}

var orbitalCenter = Array(3);

var orbitalLookupTable = Array(201);
var orbitalThetaMin;
var orbitalThetaMax;
var sphereTopology = false;

function orbitalSurface(u, v, result) {
    var chi = u*2-1;
    if (sphereTopology)
        chi = u;
    var theta = (1-Math.abs(chi))*orbitalThetaMin+Math.abs(chi)*orbitalThetaMax;
    var radius = getValueFromLookupTable(u,orbitalLookupTable)*zoomFactor;
    /*
    if (((u==0)||(u==1))&&(v==0))
        console.log("OS : "+u+" "+chi+" "+theta+" "+radius+" "+orbitalLookupTable[0]+" "+orbitalLookupTable[orbitalLookupTable.length-1]);
    */
    var phi = 2*Math.PI*v;
    if (parameters.ecorche)
        phi *= 0.85;
    result.set(radius*Math.sin(theta)*Math.cos(phi),radius*Math.sin(theta)*Math.sin(phi),radius*Math.cos(theta));
}

function sphere(u, v, result) {
    theta = Math.PI*u;
    phi = 2*Math.PI*v;
    radius = 1;
    if (parameters.type=='Complex surface')
        radius = Math.abs(PLM(theta,parameters.ell,parameters.m));
    else if (parameters.type=='Real surface') {
        if (parameters.m>=0)
            radius = Math.abs(PLM(theta,parameters.ell,parameters.m)*Math.cos(parameters.m*phi));
        else if (parameters.m<0)
            radius = Math.abs(PLM(theta,parameters.ell,parameters.m)*Math.sin(parameters.m*phi));
    }
    result.set(radius*Math.sin(theta)*Math.cos(phi),radius*Math.sin(theta)*Math.sin(phi),radius*Math.cos(theta));
}

var wColorBar = 400;
var hColorBar = 200;
var colorBarData = new Uint8Array(wColorBar*hColorBar*4);
for (var i=0; i<wColorBar; i++) {
    for (var j=0; j<hColorBar; j++) {
       var color = wavelengthToRgb(400+i*400./wColorBar);
       if ((i<5)||(wColorBar-i<5)||(j<5)||(hColorBar-j<5))
           color = [128,128,128,255];
       colorBarData[wColorBar*4*j+4*i] = color[0];
       colorBarData[wColorBar*4*j+4*i+1] = color[1];
       colorBarData[wColorBar*4*j+4*i+2] = color[2];
       colorBarData[wColorBar*4*j+4*i+3] = 255;
    }
}

var texture = new THREE.DataTexture( colorBarData, wColorBar, hColorBar, THREE.RGBAFormat );
texture.type = THREE.UnsignedByteType;
texture.needsUpdate = true; // required

// Create a scene
var scene = new THREE.Scene();

// Get canvas
canvas = document.querySelector('#canvasId');
aspectRatio = canvas.clientWidth/canvas.clientHeight;
if (autosave) {
    aspectRatio = 1;
    if (autosaveIndex==2)
        aspectRatio = 492./466;
}

// Create a camera
var camera = new THREE.PerspectiveCamera( 45, aspectRatio, 0.1, 1000 );

// Create a renderer
var renderer = new THREE.WebGLRenderer({canvas:canvas, antialias:true});
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.BasicShadowMap;
console.log("Window size : "+window.innerWidth+" "+window.innerHeight);
renderer.setSize( canvas.clientWidth, canvas.clientHeight);
if (autosave) {
    switch(autosaveIndex) {
        case 1:
            renderer.setSize(768, 768);
            break;
         case 2:
            renderer.setSize(492, 466);
            break;
        case 3:
        case 4:
            renderer.setSize(256,256);
            break;
    }
}
renderer.setClearColor( 0xffffff, 1);
// document.body.appendChild( renderer.domElement );

var sphereMesh;

var alreadyPainted = false;

var psiThreshold = 1;

camera.position.z = 5;

// Create a group of objects
var root = new THREE.Group();
root.rotation.z += -0.35*Math.PI;
root.rotation.y += 0*0.25*Math.PI;
root.rotation.x += -0.35*Math.PI;
scene.add(root);

function updateSurface() {
    console.log("Radial zeros : "+radialZeros.length);
    // Remove previous surfaces
    if (alreadyPainted) {
        while(root.children.length > 0)
            root.remove(root.children[0]); 
    }
    else
        alreadyPainted = true;
    // Create sphere texture
    var sphereTexture = new THREE.TextureLoader().load( 'images/square.png' );
    sphereMaterial = new THREE.MeshBasicMaterial( { map: sphereTexture, vertexColors: THREE.VertexColors, side:THREE.DoubleSide } );
    var vertexColorMaterial  = new THREE.MeshBasicMaterial( { vertexColors: THREE.VertexColors, side: THREE.DoubleSide } );
    // Loop on individual surfaces
    for (var iThetaRegion = 0; iThetaRegion <= legendreZeros.length; iThetaRegion++) {
        console.log("Computing angular region "+(iThetaRegion+1)+"/"+(legendreZeros.length+1));
        // Loop on colatitude regions, between two zeros of F(theta)
        // One series of surfaces in each colatitude region
        var i1, i2;
        if (iThetaRegion>0)
            i1 = Math.ceil(legendreZeros[iThetaRegion-1]*(nTable-1));
        else
            i1 = 0;
        if (iThetaRegion<legendreZeros.length)
            i2 = Math.floor(legendreZeros[iThetaRegion]*(nTable-1));
        else
            i2 = nTable-1;
        var iThetaMax = indexMaxAbs(lookupTableLegendre,i1,i2);
        var theta0 = iThetaMax/(nTable-1)*Math.PI;
        for (var iRadialRegion = 0; iRadialRegion <= radialZeros.length; iRadialRegion++) {  
            // Loop on radial regions, between two zeros of R(r)
            // One surface in each radial region
            var ir1, ir2;
            if (iRadialRegion>0)
                ir1 = Math.ceil(radialZeros[iRadialRegion-1]*(nTableRadial-1));
            else
                ir1 = 0;
            if (iRadialRegion<radialZeros.length)
                ir2 = Math.floor(radialZeros[iRadialRegion]*(nTableRadial-1));
            else
                ir2 = nTableRadial-1;
            var ir0 = indexMaxAbs(lookupTableRadial,ir1,ir2);
            var r0 = ir0/(nTableRadial-1)*maxR;
            var Rmax = lookupTableRadial[ir0];
            // Now find theta extreme values
            var iTheta1, iTheta2;
            for (iTheta1 = i1; iTheta1<iThetaMax; iTheta1++) {
                if (Math.abs(Rmax*lookupTableLegendre[iTheta1])>psiThreshold)
                    break;
            }
            for (iTheta2 = i2; iTheta2>iThetaMax; iTheta2--) {
                if (Math.abs(Rmax*lookupTableLegendre[iTheta2])>psiThreshold)
                    break;
            }
            orbitalThetaMin = (iTheta1*Math.PI)/(nTable-1);
            orbitalThetaMax = (iTheta2*Math.PI)/(nTable-1);
            
            /*
            if (((iThetaRegion == 0) && (Math.abs(Rmax*PLM(0,tabulatedEll,tabulatedM))>psiThreshold))
              ||((iThetaRegion == legendreZeros.length) && (Math.abs(Rmax*PLM(Math.PI,tabulatedEll,tabulatedM))>psiThreshold)))
            */
            if (tabulatedEll == 0)
                sphereTopology = true;
            else
                sphereTopology = false;
            if (sphereTopology) {
                orbitalThetaMin = 0;
                orbitalThetaMax = Math.PI;
            }
            
            
            orbitalLookupTable[0] = r0;
            orbitalLookupTable[orbitalLookupTable.length-1] = r0;
            // console.log("r0 = "+r0);
            var sign = 1;
            if (lookupTableRadial[ir0]<0)
                sign = -1;
            // Compute orbitalLookupTable
            // console.log("Theta interval : "+orbitalThetaMin+" "+orbitalThetaMax);
            
            var nSurfaces = 1;
            if (sphereTopology && tabulatedN>1)
                nSurfaces = 2;
            
            for (var iSurface = 0; iSurface<nSurfaces; iSurface++) {
                for (var iChi = 0; iChi<orbitalLookupTable.length; iChi++) {
                    var chi;
                    if (sphereTopology) {
                        chi = iChi/(orbitalLookupTable.length-1);
                        if (iSurface>0)
                            chi = -chi-1E-10;
                    }
                    else
                        chi = iChi/(orbitalLookupTable.length-1)*2-1;
                    var theta = (1-Math.abs(chi))*orbitalThetaMin+Math.abs(chi)*orbitalThetaMax;
                    var target = sign*Math.abs(psiThreshold/PLM(theta,tabulatedEll,tabulatedM));
                    var floatIndex;
                    if (chi<0) {
                        floatIndex = solveUsingTable(target,lookupTableRadial,ir1,ir0,sign>0);
                    }
                    else {
                        floatIndex = solveUsingTable(target,lookupTableRadial,ir0,ir2,sign<0);
                    }
                    /*
                    if ((iChi == 0)||(iChi == orbitalLookupTable.length-1)) {
                        console.log("iChi = "+iChi+", theta = "+theta+" "+target+" "+floatIndex+" "+ir1+" "+ir0+" "+ir2);
                        console.log(lookupTableRadial[Math.floor(floatIndex)]+" "+lookupTableRadial[Math.floor(floatIndex)+1])
                    }
                    */
                    orbitalLookupTable[iChi] = floatIndex/(nTableRadial-1)*maxR;
                    // console.log("iChi = "+iChi+", chi = "+chi+", result = "+floatIndex+" lookup : "+lookupTableRadial[ir1]+" "+lookupTableRadial[ir0]+" "+lookupTableRadial[ir2]+" "+target+" "+ir1+" "+ir0+" "+ir2);
                }
                if (tabulatedEll!=0) { // BUG This should not be needed to close the surface. Error in computation ?
                    orbitalLookupTable[0] = (orbitalLookupTable[0]+orbitalLookupTable[orbitalLookupTable.length-1])/2;
                    orbitalLookupTable[orbitalLookupTable.length-1] = orbitalLookupTable[0];
                }
 
                // Create a parametric surface for atomic orbital
                var sphereGeometry = new THREE.ParametricGeometry(orbitalSurface,parameters.nTheta,parameters.nPhi);
                // Compute surface color from complex phase
                for (var i=0; i<sphereGeometry.vertices.length ; i++) {
                    point = sphereGeometry.vertices[i];
                    theta = Math.atan2(Math.sqrt(point.x*point.x+point.y*point.y),point.z);
                    phi = Math.atan2(point.y, point.x);
                    var color = new THREE.Color();
                    var hue = 0;
                    if (tabulatedEll>0) {
                        psi = PLM(theta,ell,m)*Rmax;
                        if (psi>0)
                            ph = .5;
                        else
                            ph = 0;
                    } else {
                        ph = 0;
                        if (iRadialRegion%2==0)
                            ph = 0.5;
                    }
                    amplitude = .5;
                    if (parameters.type == 'Real surface') {
                        if (parameters.m>=0) {
                            if (Math.cos(parameters.m*phi)<0)
                                ph += 0.5;
                        } else {
                            if (Math.sin(parameters.m*phi)<0)
                                ph += 0.5;
                        }
                    } else {
                        ph += phi/2/Math.PI*m;
                    }
                    ph += currentTime/200;
                    hue = ph%1;
                    color.setHSL(hue,1,amplitude);
                    sphereGeometry.colors[i] = color;
                }
                // Copy vertex to face color
                var faceIndex = [ 'a', 'b', 'c', 'd' ];
                for (var i=0; i<sphereGeometry.faces.length; i++) {
                    face = sphereGeometry.faces[i];
                    // console.log("Face : "+(sqr(face.normal.x)+sqr(face.normal.y)+sqr(face.normal.z)));
                    if (face instanceof THREE.Face3)
                        nSides = 3;
                    else
                        nSides = 4;
                    for (var j=0; j<nSides; j++) {
                        var color = sphereGeometry.colors[face[faceIndex[j]]];
                        var ratio = (1+0.1*Math.max(0,(face.normal.x+face.normal.z)/Math.sqrt(2)))/1.1;
                        color.r *= ratio;
                        color.g *= ratio;
                        color.b *= ratio;
                        /*
                        var hsl = color.getHSL();
                        hsl.l = 0.3+0.4*sqr(face.normal.x);
                        color.setHSL(hsl.h, hsl.s, hsl.l);
                        */
                        face.vertexColors[j] = color;
                    }
                }

                sphereMesh = new THREE.Mesh(sphereGeometry,vertexColorMaterial);
                // sphereMesh = new THREE.Mesh(sphereGeometry,new THREE.MeshBasicMaterial());
                root.add(sphereMesh);
            }
        }
    }
}

const pointLight = new THREE.PointLight(0xFFFFFF,1.0);
pointLight.position.set(5,10,15);
scene.add(pointLight);

updateObjects();

// CONTROLS
var controls = new THREE.OrbitControls( camera, renderer.domElement );

var dataReady = false;

function updateProbaDisplay() {
    parameters.proba = parseFloat(probaRange.value);
    paramProba.innerHTML = parameters.proba.toFixed(2);
}

function updateProba() {
    updateProbaDisplay();
    if ((parameters.nPhi<150)&&(parameters.nTheta<100))
        updateData();
}

function updateData() {
    dataReady = false;
    n = parameters.n;
    ell = parameters.ell;
    m = parameters.m;
    if (!autosave) {
        parameters.nTheta = parseInt(selectMeridian.options[selectMeridian.selectedIndex].text);
        parameters.nPhi = parseInt(selectParallel.options[selectParallel.selectedIndex].text);
        parameters.ecorche = ecorcheCheckbox.checked;
        parameters.autorotate = autoRotationCheckbox.checked;
        // parameters.timeEvolution = timeEvolutionCheckbox.checked;
        updateProbaDisplay();
    }
    console.log('range value '+probaRange.value);
    console.log("updateData for |"+n+","+ell+","+m+">");
    initLegendreCoeffs(ell,m);
    initLookupTable(ell,m);
    findLegendreZeros(ell,m);
    initLaguerreCoeffs(n,ell);
    initLookupTableRadial(n,ell);
    findRadialZeros(n,ell);
    initEtaTable(n,ell);
    computePsiThreshold();
    updateSurface();
    animateCounter = 0;
    dataReady = true;
/*
    var ell = parameters.ell;
    var nR = 1000;
    var rMax = 6*n*n;
    dr = rMax/nR;
    norm2 = 0;
    for (var iR=0; iR<=nR; iR++) {
        r = iR*dr;
        var result = hydrogenRadialFunction(r,n,ell);
        norm2 += result*result*r*r*dr;
    }
    norm2 = norm2/n/n/n*4;
    console.log('Norm = '+Math.sqrt(norm2));
*/

}

function initAll() {
    updateObjects();
}

function updateObjects() {
    screen.visible = parameters.screenVisible;
}

function updateM() {
    parameters.ell = Math.max(Math.abs(parameters.m),parameters.ell);
    parameters.n = Math.max(parameters.n,parameters.ell+1);
    updateData();
}

function updateL() {
    parameters.ell = Math.max(0,parameters.ell);
    parameters.n = Math.max(parameters.n,parameters.ell+1);
    if (parameters.m<0) 
        parameters.m = Math.max(parameters.m,-parameters.ell);
    else
        parameters.m = Math.min(parameters.m,parameters.ell);
    updateData();
}

function updateN() {
    var n = parameters.n;
    if (parameters.ell>n-1) {
        parameters.ell = n-1;
        if (parameters.m<0) 
            parameters.m = Math.max(parameters.m,-parameters.ell);
        else
            parameters.m = Math.min(parameters.m,parameters.ell);
    }
    updateData();
}

function updateSelectN() {
    while (selectN.options.length>0)
        selectN.remove(0);
    for (var n = 1; n<=10; n++) {
        var element = document.createElement("option");
        element.innerText = ''+n;
        selectN.append(element);
    }
    selectN.selectedIndex = parameters.n - 1;
    updateSelectEll();
}

function updateSelectEll() {
    parameters.ell = Math.max(parameters.ell, 0);
    parameters.ell = Math.min(parameters.ell, parameters.n-1);
    while (selectEll.options.length>0)
        selectEll.remove(0);
    for (var ell = 0; ell<parameters.n; ell++) {
        var element = document.createElement("option");
        element.innerText = ''+ell;
        selectEll.append(element);
    } 
    selectEll.selectedIndex = parameters.ell;
    updateSelectM();
}

// Update number of elements in m select box
function updateSelectM() {
    if (parameters.m<0) 
        parameters.m = Math.max(parameters.m,-parameters.ell);
    else
        parameters.m = Math.min(parameters.m,parameters.ell);
    while (selectM.options.length>0)
        selectM.remove(0);
    for (var m=-parameters.ell; m<=parameters.ell; m++) {
        var element = document.createElement("option");
        element.innerText = ''+m;
        selectM.append(element);
    } 
    selectM.selectedIndex = parameters.m+parameters.ell;
}

function changeN() {
    parameters.n = selectN.selectedIndex+1;
    updateSelectEll();
    updateData();
}

function incrementN() {
    parameters.n = Math.min(10,parameters.n+1);
    selectN.selectedIndex = parameters.n-1;
    updateSelectEll();
    updateData();
}

function decrementN() {
    parameters.n = Math.max(1,parameters.n-1);
    selectN.selectedIndex = parameters.n-1;
    updateSelectEll();
    updateData();
}

function changeEll() {
    parameters.ell = selectEll.selectedIndex;
    updateSelectM();
    updateData();
}

function incrementEll() {
    parameters.ell = Math.min(parameters.n-1,parameters.ell+1);
    selectEll.selectedIndex = parameters.ell;
    updateSelectM();
    updateData();
}

function decrementEll() {
    parameters.ell = Math.max(0,parameters.ell-1);
    selectEll.selectedIndex = parameters.ell;
    updateSelectM();
    updateData();
}

function incrementM() {
    parameters.m = Math.min(10,parameters.m+1);
    if (Math.abs(parameters.m)>parameters.ell) {
        parameters.ell +=1;
        updateSelectEll();
    } else
        updateSelectM();
    updateData();
}

function decrementM() {
    parameters.m = Math.max(-10,parameters.m-1);
    if (Math.abs(parameters.m)>parameters.ell) {
        parameters.ell +=1;
        updateSelectEll();
    } else
        updateSelectM();
    updateData();
}

function decrementEll() {
    parameters.ell = Math.max(0,parameters.ell-1);
    selectEll.selectedIndex = parameters.ell;
    updateSelectM();
    updateData();
}

function changeM() {
    parameters.m = selectM.selectedIndex-parameters.ell;
    updateData();
}

function changePlot() {
    parameters.autorotate = autoRotationCheckbox.checked;
    updateData();
}

function computePsiThreshold() {
    psiThreshold = solveUsingTable(1-parameters.proba,lookupEtaTable)/(lookupEtaTable.length-1)*maxAbs(lookupTableRadial)*maxAbs(lookupTableLegendre);
    console.log('New eta value '+psiThreshold);
}

function manageGalleryScale() {
    var galleryScaleCheckbox = document.getElementById("checkboxScaleId");
    var galleryEcorcheCheckbox = document.getElementById("checkboxEcorcheId");
    var g1 = document.getElementById("gallery1Id");
    var g2 = document.getElementById("gallery2Id");
    var g3 = document.getElementById("gallery3Id");
    var g4 = document.getElementById("gallery4Id");
    if (galleryScaleCheckbox.checked) {
        g1.style.display = "none";
        g3.style.display = "none";
        if (galleryEcorcheCheckbox.checked) {
            g2.style.display = "none";
            g4.style.display = "block";
        } else {
            g4.style.display = "none";
            g2.style.display = "block";
        }
    } else {
        g2.style.display = "none";
        g4.style.display = "none";
        if (galleryEcorcheCheckbox.checked) {
            g1.style.display = "none";
            g3.style.display = "block";
        } else {
            g3.style.display = "none";
            g1.style.display = "block";
        }
    }
    
}

function buildFilename() {
    var s = "psi"+format(tabulatedN,2)+""+format(tabulatedEll,2);
    if (tabulatedM<0)
        s += "-";
    else
        s += "+";
    s += format(Math.abs(tabulatedM),2);
    return s;
}

updateSelectN();
updateData();

var animateCounter = 0;

var fileIndex = 0;

var animate = function (time) {
    requestAnimationFrame(animate);
    // processAnimation();
    if (dataReady) {
        animateCounter += 1;
        renderer.render(scene, camera );
        if (animateCounter>=200) {
            if (autosave) {
                console.log('autosave processing shot'+format(fileIndex,5)+' '+animateCounter);
                switch(autosaveIndex) {
                    case 1:
                        if (animateCounter==10)
                            saveImage(buildFilename());
                        break;
                    case 2:
                        if (fileIndex<200) {
                            saveImage("shot"+format(fileIndex,5));
                            fileIndex += 1;
                        }
                        break;
                    case 3:
                        saveImage("psi642p"+(fileIndex+1)+'e');
                        parameters.proba = Math.min(0.9, parameters.proba+.1);
                        updateData();
                        fileIndex += 1;
                        if (fileIndex>8)
                            autosave = false;
                        break;
                    case 4:
                        var s = 'psi'+parameters.n+parameters.ell;
                        if (parameters.m>=0)
                            s = s+'p';
                        else
                            s = s+'m';
                        s = s+Math.abs(parameters.m); // +'se';
                        saveImage(s);
                        parameters.m += 1;
                        if (parameters.m>parameters.ell) {
                            parameters.ell += 1;
                            if (parameters.ell>=parameters.n) {
                                parameters.n += 1;
                                // zoomFactor = 1.0/parameters.n/parameters.n;
                                parameters.ell = 0;
                                parameters.m = 0;
                            } else
                                parameters.m = -parameters.ell;
                        }
                        updateData();
                        fileIndex += 1;
                        if (parameters.n>5)
                            autosave = false;
                        break;
                }
            }
            if (parameters.autorotate)
                root.rotation.x += Math.PI/100;
            if (parameters.timeEvolution) {
                currentTime += 1;
                updateSurface();
            }
        }
    }
};

renderer.render(scene,camera);
manageGalleryScale();

// var canvas = document.getElementsByTagName("canvas")[0];

document.addEventListener("keydown", onDocumentKeyDown, false);

animate();