
/*
    pbrt source code Copyright(c) 1998-2012 Matt Pharr and Greg Humphreys.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */


// accelerators/kdtreeaccel.cpp*
#include "stdafx.h"
#include "accelerators/kdtreeaccel.h"
#include "paramset.h"

// KdTreeAccel Local Declarations
struct KdAccelNode {
    // KdAccelNode Methods
    void initLeaf(uint32_t *primNums, int np, MemoryArena &arena);
    void initInterior(uint32_t axis, uint32_t ac, float s) {
        split = s;
        flags = axis;
        aboveChild |= (ac << 2);
    }
    float SplitPos() const { return split; }
    uint32_t nPrimitives() const { return nPrims >> 2; }
    uint32_t SplitAxis() const { return flags & 3; }
    bool IsLeaf() const { return (flags & 3) == 3; }
    uint32_t AboveChild() const { return aboveChild >> 2; }
	void PromoteNode(int n) { 
		if (!IsLeaf())
			aboveChild += (n << 2);
		else {
			if ((nPrims >> 2) == 1)
				onePrimitive += n;
			else
				for (uint32_t i = 0; i < (nPrims >> 2); ++i)
					primitives[i] += n;
		}
	}
    union {
        float split;            // Interior
        uint32_t onePrimitive;  // Leaf
        uint32_t *primitives;   // Leaf
    };

private:
    union {
        uint32_t flags;         // Both
        uint32_t nPrims;        // Leaf
        uint32_t aboveChild;    // Interior
    };
};


struct BoundEdge {
    // BoundEdge Public Methods
    BoundEdge() { }
    BoundEdge(float tt, int pn, bool starting) {
        t = tt;
        primNum = pn;
        type = starting ? START : END;
    }
    bool operator<(const BoundEdge &e) const {
        if (t == e.t)
            return (int)type < (int)e.type;
        else return t < e.t;
    }
    float t;
    int primNum;
    enum { START, END } type;
};


struct KdTreePrimitiveRefineTask : public Task {
	KdTreePrimitiveRefineTask(const vector<Reference<Primitive> > &_p,
		vector<Reference<Primitive> > &_primitives,
		uint32_t _startIdx, uint32_t _endIdx)
		: p(_p), startIdx(_startIdx), endIdx(_endIdx), primitives(_primitives) {}
	void Run(){
		for (uint32_t i = startIdx; i < endIdx; ++i)
			p[i]->FullyRefine(primitives);
	};
	const vector<Reference<Primitive> > &p;
	vector<Reference<Primitive> > &primitives;
	uint32_t startIdx;
	uint32_t endIdx;
};


struct KdTreeComputeBoundTask : public Task {
	KdTreeComputeBoundTask(const vector<Reference<Primitive> > &_primitives,
		vector<BBox > &_primBounds, BBox &_bounds, uint32_t *_primNums, 
		uint32_t _startIdx, uint32_t _endIdx)
		: primitives(_primitives), 
		startIdx(_startIdx), endIdx(_endIdx), 
		primBounds(_primBounds), bounds(_bounds), primNums(_primNums){}
	void Run(){
		primBounds.reserve(endIdx - startIdx);
		for (uint32_t i = startIdx; i < endIdx; ++i)
		{
			BBox b = primitives[i]->WorldBound();
			bounds = Union(bounds, b);
			primBounds.push_back(b);
			primNums[i] = i;
		}
	};
	const vector<Reference<Primitive> > &primitives;
	vector<BBox > &primBounds;
	BBox &bounds;
	uint32_t startIdx;
	uint32_t endIdx;
	uint32_t *primNums;
};


struct KdTreeBuildSubTreeTask : public Task{
public:
	vector<Reference<Primitive> > &prims;
	KdTreeAccel *subKdTree;
	int depth, originNodeIdx;
	KdTreeBuildSubTreeTask(vector<Reference<Primitive> > &_prims, int _depth, int _originNodeIdx)
	:prims(_prims), depth(_depth), originNodeIdx(_originNodeIdx){}

	~KdTreeBuildSubTreeTask() {
		prims.~vector();
		delete &prims;
		subKdTree->~KdTreeAccel();
		delete subKdTree;
	}

	void Run(){
		subKdTree = CreateSubKdTreeAccelerator(prims, depth);
	}
};


KdTreeAccel *testptr;

// KdTreeAccel Method Definitions
KdTreeAccel::KdTreeAccel(const vector<Reference<Primitive> > &p, bool pEntry, 
                         int icost, int tcost, float ebonus, int maxp,
                         int md, int ws)
    : isectCost(icost), parallelEntry(pEntry), traversalCost(tcost), maxPrims(maxp), maxDepth(md),
      emptyBonus(ebonus), workloadMaxSize(ws) {
	PBRT_KDTREE_STARTED_CONSTRUCTION(this, p.size());
		
	// Thread context initialization
	threadNum = NumSystemCores();
	vector<Task *> tasks;
	tasks.resize(threadNum);

	if (parallelEntry)
	{
		// Parallelly refine primitives
		vector<Reference<Primitive> > *_thread_primitives = new vector<Reference<Primitive> >[threadNum];
		for (int i = 0; i < threadNum; ++i)
			tasks[i] = new KdTreePrimitiveRefineTask(p, _thread_primitives[i],
			p.size() * i / threadNum, p.size() * (i + 1) / threadNum);
		EnqueueTasks(tasks);
		WaitForAllTasks();
		for (uint32_t i = 0; i < threadNum; ++i)
		{
			delete tasks[i];
			primitives.insert(primitives.end(), _thread_primitives[i].begin(), _thread_primitives[i].end());
			_thread_primitives[i].~vector();
		}
		delete[] _thread_primitives;
	}
	// They've been refined
	else
		//primitives.insert(primitives.end(), p.begin(), p.end());
		for (uint32_t i = 0; i < p.size(); ++i)
			p[i]->FullyRefine(primitives);

	// Build kd-tree for accelerator
	nextFreeNode = nAllocedNodes = 0;
	if (maxDepth < 0)
		maxDepth = Round2Int(8 + 1.3f * Log2Int(float(primitives.size())));

	// Parallelly compute bounds for kd-tree construction
	// and initialize _primNums_ for kd-tree construction
	uint32_t *primNums = new uint32_t[primitives.size()];
	vector<BBox> primBounds;
	primBounds.reserve(primitives.size());
	
	if (parallelEntry){
		vector<BBox> *_thread_primBounds = new vector<BBox>[threadNum];
		BBox *_thread_bounds = new BBox[threadNum];
		for (int i = 0; i < threadNum; ++i)
			tasks[i] = new KdTreeComputeBoundTask(primitives, _thread_primBounds[i], _thread_bounds[i], primNums,
				primitives.size() * i / threadNum, primitives.size() * (i + 1) / threadNum);
		EnqueueTasks(tasks);
		WaitForAllTasks();
		// Merge bounds
		for (uint32_t i = 0; i < threadNum; ++i)
		{
			delete tasks[i];
			primBounds.insert(primBounds.end(), _thread_primBounds[i].begin(), _thread_primBounds[i].end());
			bounds = Union(bounds, _thread_bounds[i]);
			_thread_primBounds[i].~vector();
		}
		delete[] _thread_primBounds;
		delete[] _thread_bounds;
	}
	else
		for (uint32_t i = 0; i < primitives.size(); ++i)
		{
			BBox b = primitives[i]->WorldBound();
			bounds = Union(bounds, b);
			primBounds.push_back(b);
			primNums[i] = i;
		}

    // Allocate working memory for kd-tree construction
    BoundEdge *edges[3];
    for (int i = 0; i < 3; ++i)
        edges[i] = new BoundEdge[2*primitives.size()];
    uint32_t *prims0 = new uint32_t[primitives.size()];
    uint32_t *prims1 = new uint32_t[(maxDepth+1) * primitives.size()];

    // Start recursive construction of kd-tree
	// modified to generate buildSubTree tasks
	if (parallelEntry)
		tasks.clear();

	buildTree(0, bounds, primBounds, primNums, primitives.size(),
		maxDepth, edges, prims0, prims1, tasks);

	if (parallelEntry) {
		EnqueueTasks(tasks);
		WaitForAllTasks();
		
		int taskIOffset[2] = { -1, -1 };
		int nodesOffset = 0, newNodesOffset = 0;
		int newNextFreeNode = 0, newNAllocedNodes = 0;
		KdAccelNode *newNodes;
		
		for (uint32_t i = 0; i < tasks.size(); ++i)
			newNAllocedNodes += (dynamic_cast<KdTreeBuildSubTreeTask *>(tasks[i]))->subKdTree->GetNodeNum();
			
		newNAllocedNodes += GetNodeNum();
		newNAllocedNodes -= tasks.size();

		newNodes = AllocAligned<KdAccelNode>(newNAllocedNodes);

		for (uint32_t i = 0; i < tasks.size(); ++i) {
			// add and promote nodes interval
			taskIOffset[1] = (dynamic_cast<KdTreeBuildSubTreeTask *>(tasks[i]))->originNodeIdx;
			int nodesInterval = taskIOffset[1] - taskIOffset[0] - 1;
			memcpy(newNodes + newNodesOffset, nodes + nodesOffset, nodesInterval * sizeof(KdAccelNode));
			newNodesOffset += nodesInterval;
			nodesOffset += nodesInterval;
			taskIOffset[0] = taskIOffset[1];

			for (uint32_t j = newNodesOffset; j < newNodesOffset + nodesInterval; ++j)
				newNodes[j].PromoteNode(newNodesOffset - nodesOffset);

			// add and promote task interval
			int taskInterval = (dynamic_cast<KdTreeBuildSubTreeTask *>(tasks[i]))->subKdTree->GetNodeNum();
			if (taskInterval != 1)
				int k = 0;
			
			memcpy(newNodes + newNodesOffset, 
				(dynamic_cast<KdTreeBuildSubTreeTask *>(tasks[i]))->subKdTree->GetNodes(), 
				taskInterval * sizeof(KdAccelNode));
			newNodesOffset += taskInterval;
			nodesOffset += 1;

			for (uint32_t j = newNodesOffset - taskInterval; j < newNodesOffset; ++j)
				newNodes[j].PromoteNode(newNodesOffset);

			//delete tasks[i];
			//if (i == 5)
			//	testptr = (dynamic_cast<KdTreeBuildSubTreeTask *>(tasks[i]))->subKdTree;
		}
		//newNodes = (dynamic_cast<KdTreeBuildSubTreeTask *>(tasks[100]))->subKdTree->nodes;
		//primitives = (dynamic_cast<KdTreeBuildSubTreeTask *>(tasks[100]))->subKdTree->primitives;

		// replace with new nodes
		FreeAligned(nodes);
		nodes = newNodes;
	}

    // Free working memory for kd-tree construction
    delete[] primNums;
    for (int i = 0; i < 3; ++i)
        delete[] edges[i];
    delete[] prims0;
    delete[] prims1;
    PBRT_KDTREE_FINISHED_CONSTRUCTION(this);
}


void KdAccelNode::initLeaf(uint32_t *primNums, int np,
                           MemoryArena &arena) {
    flags = 3;
    nPrims |= (np << 2);
    // Store primitive ids for leaf node
    if (np == 0)
        onePrimitive = 0;
    else if (np == 1)
        onePrimitive = primNums[0];
    else {
        primitives = arena.Alloc<uint32_t>(np);
        for (int i = 0; i < np; ++i)
            primitives[i] = primNums[i];
    }
}


KdTreeAccel::~KdTreeAccel() {
    FreeAligned(nodes);
}


void KdTreeAccel::buildTree(int nodeNum, const BBox &nodeBounds,
	const vector<BBox> &allPrimBounds, uint32_t *primNums,
	int nPrimitives, int depth, BoundEdge *edges[3],
	uint32_t *prims0, uint32_t *prims1, vector<Task *> &tasks, int badRefines) {
	Assert(nodeNum == nextFreeNode);
	// Get next free node from _nodes_ array
	if (nextFreeNode == nAllocedNodes)
	{
		int nAlloc = max(2 * nAllocedNodes, 512);
		KdAccelNode *n = AllocAligned<KdAccelNode>(nAlloc);
		if (nAllocedNodes > 0)
		{
			memcpy(n, nodes, nAllocedNodes * sizeof(KdAccelNode));
			FreeAligned(nodes);
		}
		nodes = n;
		nAllocedNodes = nAlloc;
	}
	++nextFreeNode;

	// Initialize leaf node if termination criteria met
	if (nPrimitives <= maxPrims || depth == 0)
	{
		PBRT_KDTREE_CREATED_LEAF(nPrimitives, maxDepth - depth);
		nodes[nodeNum].initLeaf(primNums, nPrimitives, arena);
		return;
	}

	// Initialize interior node and continue recursion

	// Choose split axis position for interior node
	int bestAxis = -1, bestOffset = -1;
	float bestCost = INFINITY;
	float oldCost = isectCost * float(nPrimitives);
	float totalSA = nodeBounds.SurfaceArea();
	float invTotalSA = 1.f / totalSA;
	Vector d = nodeBounds.pMax - nodeBounds.pMin;

	// Choose which axis to split along
	uint32_t axis = nodeBounds.MaximumExtent();
	int retries = 0;
retrySplit:

	// Initialize edges for _axis_
	for (int i = 0; i < nPrimitives; ++i)
	{
		int pn = primNums[i];
		const BBox &bbox = allPrimBounds[pn];
		edges[axis][2 * i] = BoundEdge(bbox.pMin[axis], pn, true);
		edges[axis][2 * i + 1] = BoundEdge(bbox.pMax[axis], pn, false);
	}
	sort(&edges[axis][0], &edges[axis][2 * nPrimitives]);

	// Compute cost of all splits for _axis_ to find best
	int nBelow = 0, nAbove = nPrimitives;
	for (int i = 0; i < 2 * nPrimitives; ++i)
	{
		if (edges[axis][i].type == BoundEdge::END) --nAbove;
		float edget = edges[axis][i].t;
		if (edget > nodeBounds.pMin[axis] &&
			edget < nodeBounds.pMax[axis])
		{
			// Compute cost for split at _i_th edge
			uint32_t otherAxis0 = (axis + 1) % 3, otherAxis1 = (axis + 2) % 3;
			float belowSA = 2 * (d[otherAxis0] * d[otherAxis1] +
				(edget - nodeBounds.pMin[axis]) *
				(d[otherAxis0] + d[otherAxis1]));
			float aboveSA = 2 * (d[otherAxis0] * d[otherAxis1] +
				(nodeBounds.pMax[axis] - edget) *
				(d[otherAxis0] + d[otherAxis1]));
			float pBelow = belowSA * invTotalSA;
			float pAbove = aboveSA * invTotalSA;
			float eb = (nAbove == 0 || nBelow == 0) ? emptyBonus : 0.f;
			float cost = traversalCost +
				isectCost * (1.f - eb) * (pBelow * nBelow + pAbove * nAbove);

			// Update best split if this is lowest cost so far
			if (cost < bestCost)
			{
				bestCost = cost;
				bestAxis = axis;
				bestOffset = i;
			}
		}
		if (edges[axis][i].type == BoundEdge::START) ++nBelow;
	}
	Assert(nBelow == nPrimitives && nAbove == 0);

	// Create leaf if no good splits were found
	if (bestAxis == -1 && retries < 2)
	{
		++retries;
		axis = (axis + 1) % 3;
		goto retrySplit;
	}
	if (bestCost > oldCost) ++badRefines;
	if ((bestCost > 4.f * oldCost && nPrimitives < 16) ||
		bestAxis == -1 || badRefines == 3)
	{
		PBRT_KDTREE_CREATED_LEAF(nPrimitives, maxDepth - depth);
		nodes[nodeNum].initLeaf(primNums, nPrimitives, arena);
		return;
	}

	// Classify primitives with respect to split
	int n0 = 0, n1 = 0;
	for (int i = 0; i < bestOffset; ++i)
		if (edges[bestAxis][i].type == BoundEdge::START)
			prims0[n0++] = edges[bestAxis][i].primNum;
	for (int i = bestOffset + 1; i < 2 * nPrimitives; ++i)
		if (edges[bestAxis][i].type == BoundEdge::END)
			prims1[n1++] = edges[bestAxis][i].primNum;

	// Recursively initialize children nodes
	float tsplit = edges[bestAxis][bestOffset].t;
	PBRT_KDTREE_CREATED_INTERIOR_NODE(bestAxis, tsplit);
	BBox bounds0 = nodeBounds, bounds1 = nodeBounds;
	bounds0.pMax[bestAxis] = bounds1.pMin[bestAxis] = tsplit;
	
	//if (parallelEntry) {
	if ((n0 < workloadMaxSize) && (n0 > maxPrims) && parallelEntry) {
		vector<Reference<Primitive> > *prims = new vector<Reference<Primitive> >;
		for (int i = 0; i < n0; i++)
			prims->push_back(primitives[i]);
		tasks.push_back(new KdTreeBuildSubTreeTask(*prims, 0, nodeNum + 1));
		++nextFreeNode;
		// Get next free node from _nodes_ array
		if (nextFreeNode == nAllocedNodes)
		{
			int nAlloc = max(2 * nAllocedNodes, 512);
			KdAccelNode *n = AllocAligned<KdAccelNode>(nAlloc);
			if (nAllocedNodes > 0)
			{
				memcpy(n, nodes, nAllocedNodes * sizeof(KdAccelNode));
				FreeAligned(nodes);
			}
			nodes = n;
			nAllocedNodes = nAlloc;
		}
	}
	else
		buildTree(nodeNum + 1, bounds0,
			allPrimBounds, prims0, n0, depth - 1, edges,
			prims0, prims1 + nPrimitives, tasks, badRefines);
	
	uint32_t aboveChild = nextFreeNode;	
	nodes[nodeNum].initInterior(bestAxis, aboveChild, tsplit);

	//if (parallelEntry) {
	if ((n1 < workloadMaxSize) && (n1 > maxPrims) && parallelEntry) {
		vector<Reference<Primitive> > *prims = new vector<Reference<Primitive> >;
		for (int i = 0; i < n1; i++)
			prims->push_back(primitives[i]);
		tasks.push_back(new KdTreeBuildSubTreeTask(*prims, 0, aboveChild));
		++nextFreeNode;
		// Get next free node from _nodes_ array
		if (nextFreeNode == nAllocedNodes)
		{
			int nAlloc = max(2 * nAllocedNodes, 512);
			KdAccelNode *n = AllocAligned<KdAccelNode>(nAlloc);
			if (nAllocedNodes > 0)
			{
				memcpy(n, nodes, nAllocedNodes * sizeof(KdAccelNode));
				FreeAligned(nodes);
			}
			nodes = n;
			nAllocedNodes = nAlloc;
		}
	}
	else
		buildTree(aboveChild, bounds1, allPrimBounds, prims1, n1,
			depth - 1, edges, prims0, prims1 + nPrimitives, tasks, badRefines);
}


bool KdTreeAccel::Intersect(const Ray &ray,
                            Intersection *isect) const {
    PBRT_KDTREE_INTERSECTION_TEST(const_cast<KdTreeAccel *>(this), const_cast<Ray *>(&ray));
    // Compute initial parametric range of ray inside kd-tree extent
    float tmin, tmax;
    if (!bounds.IntersectP(ray, &tmin, &tmax))
    {
        PBRT_KDTREE_RAY_MISSED_BOUNDS();
        return false;
    }

    // Prepare to traverse kd-tree for ray
    Vector invDir(1.f/ray.d.x, 1.f/ray.d.y, 1.f/ray.d.z);
#define MAX_TODO 64
    KdToDo todo[MAX_TODO];
    int todoPos = 0;

    // Traverse kd-tree nodes in order for ray
    bool hit = false;
    const KdAccelNode *node = &nodes[0];
    while (node != NULL) {
        // Bail out if we found a hit closer than the current node
        if (ray.maxt < tmin) break;
        if (!node->IsLeaf()) {
            PBRT_KDTREE_INTERSECTION_TRAVERSED_INTERIOR_NODE(const_cast<KdAccelNode *>(node));
            // Process kd-tree interior node

            // Compute parametric distance along ray to split plane
            int axis = node->SplitAxis();
            float tplane = (node->SplitPos() - ray.o[axis]) * invDir[axis];

            // Get node children pointers for ray
            const KdAccelNode *firstChild, *secondChild;
            int belowFirst = (ray.o[axis] <  node->SplitPos()) ||
                             (ray.o[axis] == node->SplitPos() && ray.d[axis] <= 0);
            if (belowFirst) {
                firstChild = node + 1;
                secondChild = &nodes[node->AboveChild()];
            }
            else {
                firstChild = &nodes[node->AboveChild()];
                secondChild = node + 1;
            }

            // Advance to next child node, possibly enqueue other child
            if (tplane > tmax || tplane <= 0)
                node = firstChild;
            else if (tplane < tmin)
                node = secondChild;
            else {
                // Enqueue _secondChild_ in todo list
                todo[todoPos].node = secondChild;
                todo[todoPos].tmin = tplane;
                todo[todoPos].tmax = tmax;
                ++todoPos;
                node = firstChild;
                tmax = tplane;
            }
        }
        else {
            PBRT_KDTREE_INTERSECTION_TRAVERSED_LEAF_NODE(const_cast<KdAccelNode *>(node), node->nPrimitives());
            // Check for intersections inside leaf node
            uint32_t nPrimitives = node->nPrimitives();
            if (nPrimitives == 1) {
                const Reference<Primitive> &prim = primitives[node->onePrimitive];
                // Check one primitive inside leaf node
                PBRT_KDTREE_INTERSECTION_PRIMITIVE_TEST(const_cast<Primitive *>(prim.GetPtr()));
                if (prim->Intersect(ray, isect))
                {
                    PBRT_KDTREE_INTERSECTION_HIT(const_cast<Primitive *>(prim.GetPtr()));
                    hit = true;
                }
            }
            else {
                uint32_t *prims = node->primitives;
                for (uint32_t i = 0; i < nPrimitives; ++i) {
                    const Reference<Primitive> &prim = primitives[prims[i]];
                    // Check one primitive inside leaf node
                    PBRT_KDTREE_INTERSECTION_PRIMITIVE_TEST(const_cast<Primitive *>(prim.GetPtr()));
                    if (prim->Intersect(ray, isect))
                    {
                        PBRT_KDTREE_INTERSECTION_HIT(const_cast<Primitive *>(prim.GetPtr()));
                        hit = true;
                    }
                }
            }

            // Grab next node to process from todo list
            if (todoPos > 0) {
                --todoPos;
                node = todo[todoPos].node;
                tmin = todo[todoPos].tmin;
                tmax = todo[todoPos].tmax;
            }
            else
                break;
        }
    }
    PBRT_KDTREE_INTERSECTION_FINISHED();
    return hit;
}


bool KdTreeAccel::IntersectP(const Ray &ray) const {
    PBRT_KDTREE_INTERSECTIONP_TEST(const_cast<KdTreeAccel *>(this), const_cast<Ray *>(&ray));
    // Compute initial parametric range of ray inside kd-tree extent
    float tmin, tmax;
    if (!bounds.IntersectP(ray, &tmin, &tmax))
    {
        PBRT_KDTREE_RAY_MISSED_BOUNDS();
        return false;
    }

    // Prepare to traverse kd-tree for ray
    Vector invDir(1.f/ray.d.x, 1.f/ray.d.y, 1.f/ray.d.z);
#define MAX_TODO 64
    KdToDo todo[MAX_TODO];
    int todoPos = 0;
    const KdAccelNode *node = &nodes[0];
    while (node != NULL) {
        if (node->IsLeaf()) {
            PBRT_KDTREE_INTERSECTIONP_TRAVERSED_LEAF_NODE(const_cast<KdAccelNode *>(node), node->nPrimitives());
            // Check for shadow ray intersections inside leaf node
            uint32_t nPrimitives = node->nPrimitives();
            if (nPrimitives == 1) {
                const Reference<Primitive> &prim = primitives[node->onePrimitive];
                PBRT_KDTREE_INTERSECTIONP_PRIMITIVE_TEST(const_cast<Primitive *>(prim.GetPtr()));
                if (prim->IntersectP(ray)) {
                    PBRT_KDTREE_INTERSECTIONP_HIT(const_cast<Primitive *>(prim.GetPtr()));
                    return true;
                }
            }
            else {
                uint32_t *prims = node->primitives;
                for (uint32_t i = 0; i < nPrimitives; ++i) {
                    const Reference<Primitive> &prim = primitives[prims[i]];
                    PBRT_KDTREE_INTERSECTIONP_PRIMITIVE_TEST(const_cast<Primitive *>(prim.GetPtr()));
                    if (prim->IntersectP(ray)) {
                        PBRT_KDTREE_INTERSECTIONP_HIT(const_cast<Primitive *>(prim.GetPtr()));
                        return true;
                    }
                }
            }

            // Grab next node to process from todo list
            if (todoPos > 0) {
                --todoPos;
                node = todo[todoPos].node;
                tmin = todo[todoPos].tmin;
                tmax = todo[todoPos].tmax;
            }
            else
                break;
        }
        else {
            PBRT_KDTREE_INTERSECTIONP_TRAVERSED_INTERIOR_NODE(const_cast<KdAccelNode *>(node));
            // Process kd-tree interior node

            // Compute parametric distance along ray to split plane
            int axis = node->SplitAxis();
            float tplane = (node->SplitPos() - ray.o[axis]) * invDir[axis];

            // Get node children pointers for ray
            const KdAccelNode *firstChild, *secondChild;
            int belowFirst = (ray.o[axis] <  node->SplitPos()) ||
                             (ray.o[axis] == node->SplitPos() && ray.d[axis] <= 0);
            if (belowFirst) {
                firstChild = node + 1;
                secondChild = &nodes[node->AboveChild()];
            }
            else {
                firstChild = &nodes[node->AboveChild()];
                secondChild = node + 1;
            }

            // Advance to next child node, possibly enqueue other child
            if (tplane > tmax || tplane <= 0)
                node = firstChild;
            else if (tplane < tmin)
                node = secondChild;
            else {
                // Enqueue _secondChild_ in todo list
                todo[todoPos].node = secondChild;
                todo[todoPos].tmin = tplane;
                todo[todoPos].tmax = tmax;
                ++todoPos;
                node = firstChild;
                tmax = tplane;
            }
        }
    }
    PBRT_KDTREE_INTERSECTIONP_MISSED();
    return false;
}


KdTreeAccel *CreateKdTreeAccelerator(const vector<Reference<Primitive> > &prims,
        const ParamSet &ps) {
    int isectCost = ps.FindOneInt("intersectcost", 80);
    int travCost = ps.FindOneInt("traversalcost", 1);
    float emptyBonus = ps.FindOneFloat("emptybonus", 0.5f);
    int maxPrims = ps.FindOneInt("maxprims", 1);
    int maxDepth = ps.FindOneInt("maxdepth", -1);
    return new KdTreeAccel(prims, true, isectCost, travCost,
        emptyBonus, maxPrims, maxDepth);
	//KdTreeAccel(prims, true, isectCost, travCost,
	//	emptyBonus, maxPrims, maxDepth);
	//return testptr;
}


KdTreeAccel *CreateSubKdTreeAccelerator(const vector<Reference<Primitive> > &prims, int maxDepth) {
	int isectCost = 80;
	int travCost = 1;
	float emptyBonus = 0.5f;
	int maxPrims = 1;
	return new KdTreeAccel(prims, false, isectCost, travCost,
		emptyBonus, maxPrims, maxDepth);
}


