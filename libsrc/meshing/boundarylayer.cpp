#include <mystdlib.h>
#include "meshing.hpp"
#include "meshing2.hpp"
#include "delaunay2d.hpp"
#include "global.hpp"
#include "../geom2d/csg2d.hpp"

namespace netgen
{

   void InsertVirtualBoundaryLayer (Mesh & mesh)
   {
      cout << "Insert virt. b.l." << endl;

      int surfid;

      cout << "Boundary Nr:";
      cin >> surfid;

      int i;
      int np = mesh.GetNP();

      cout << "Old NP: " << mesh.GetNP() << endl;
      cout << "Trigs: " << mesh.GetNSE() << endl;

      NgBitArray bndnodes(np);
      NgArray<int> mapto(np);

      bndnodes.Clear();
      for (i = 1; i <= mesh.GetNSeg(); i++)
      {
         int snr = mesh.LineSegment(i).edgenr;
         cout << "snr = " << snr << endl;
         if (snr == surfid)
         {
            bndnodes.Set (mesh.LineSegment(i)[0]);
            bndnodes.Set (mesh.LineSegment(i)[1]);
         }
      }
      for (i = 1; i <= mesh.GetNSeg(); i++)
      {
         int snr = mesh.LineSegment(i).edgenr;
         if (snr != surfid)
         {
            bndnodes.Clear (mesh.LineSegment(i)[0]);
            bndnodes.Clear (mesh.LineSegment(i)[1]);
         }
      }

      for (i = 1; i <= np; i++)
        {
          if (bndnodes.Test(i))
            mapto.Elem(i) = mesh.AddPoint (mesh.Point (i));
          else
            mapto.Elem(i) = 0;
        }

      for (i = 1; i <= mesh.GetNSE(); i++)
      {
         Element2d & el = mesh.SurfaceElement(i);
         for (int j = 1; j <= el.GetNP(); j++)
            if (mapto.Get(el.PNum(j)))
               el.PNum(j) = mapto.Get(el.PNum(j));
      }


      int nq = 0;
      for (i = 1; i <= mesh.GetNSeg(); i++)
      {
         int snr = mesh.LineSegment(i).edgenr;
         if (snr == surfid)
         {
            int p1 = mesh.LineSegment(i)[0];
            int p2 = mesh.LineSegment(i)[1];
            int p3 = mapto.Get (p1);
            if (!p3) p3 = p1;
            int p4 = mapto.Get (p2);
            if (!p4) p4 = p2;

            Element2d el(QUAD);
            el.PNum(1) = p1;
            el.PNum(2) = p2;
            el.PNum(3) = p3;
            el.PNum(4) = p4;
            el.SetIndex (2);
            mesh.AddSurfaceElement (el);
            nq++;
         }
      }

      cout << "New NP: " << mesh.GetNP() << endl;
      cout << "Quads: " << nq << endl;
   }

  // depending on the geometry type, the mesh contains segments multiple times (once for each face)
  bool HaveSingleSegments( const Mesh & mesh )
  {
      auto& topo = mesh.GetTopology();
      NgArray<SurfaceElementIndex> surf_els;

      for(auto segi : Range(mesh.LineSegments()))
      {
          mesh.GetTopology().GetSegmentSurfaceElements(segi+1, surf_els);
          if(surf_els.Size()<2)
              continue;

          auto seg = mesh[segi];
          auto pi0 = min(seg[0], seg[1]);
          auto pi1 = max(seg[0], seg[1]);
          auto p0_segs = topo.GetVertexSegments(seg[0]);

          for(auto segi_other : p0_segs)
          {
              if(segi_other == segi)
                  continue;

              auto seg_other = mesh[segi_other];
              auto pi0_other = min(seg_other[0], seg_other[1]);
              auto pi1_other = max(seg_other[0], seg_other[1]);
              if( pi0_other == pi0 && pi1_other == pi1 )
                  return false;
          }

          // found segment with multiple adjacent surface elements but no other segments with same points -> have single segments
          return true;
      }

      return true;
  }

  // duplicates segments (and sets seg.si accordingly) to have a unified data structure for all geometry types
  Array<Segment> BuildSegments( Mesh & mesh )
  {
      Array<Segment> segments;
      auto& topo = mesh.GetTopology();

      NgArray<SurfaceElementIndex> surf_els;

      for(auto segi : Range(mesh.LineSegments()))
      {
          auto seg = mesh[segi];
          mesh.GetTopology().GetSegmentSurfaceElements(segi+1, surf_els);
          for(auto seli : surf_els)
          {
              const auto & sel = mesh[seli];
              seg.si = sel.GetIndex();

              auto np = sel.GetNP();
              for(auto i : Range(np))
              {
                  if(sel[i] == seg[0])
                  {
                      if(sel[(i+1)%np] != seg[1])
                          swap(seg[0], seg[1]);
                      break;
                  }
              }

              // seg.edgenr = topo.GetEdge(segi)+1;
              segments.Append(seg);
          }
      }
      return segments;
  }

  void MergeAndAddSegments( Mesh & mesh, FlatArray<Segment> new_segments)
  {
      INDEX_2_HASHTABLE<bool> already_added( 2*new_segments.Size() );

      for(auto & seg : new_segments)
      {
          INDEX_2 i2 (seg[0], seg[1]);
          i2.Sort();

          if(!already_added.Used(i2))
          {
              mesh.AddSegment(seg);
              already_added.Set(i2, true);
          }
      }
  }

  void InterpolateSurfaceGrowthVectors(const Mesh & mesh, const BoundaryLayerParameters& blp, int fd_old, FlatArray<Vec<3>, PointIndex> growthvectors, const Table<SurfaceElementIndex, PointIndex> & p2sel)
  {
    auto np = mesh.GetNP();
    // interpolate growth vectors at inner surface points from surrounding edge points
    Array<Point<2>, PointIndex> delaunay_points(np);
    Array<PointIndex, PointIndex> pmap(np); // maps duplicated points

    Array<SurfaceElementIndex> surface_els;
    Array<PointIndex> edge_points;
    Array<PointIndex> surface_points;
    for(auto facei : Range(1, fd_old+1))
      {
        if(!blp.surfid.Contains(facei))
            continue;

        edge_points.SetSize(0);
        surface_points.SetSize(0);
        surface_els.SetSize(0);
        delaunay_points.SetSize(np);
        pmap.SetSize(np);
        mesh.GetSurfaceElementsOfFace (facei, surface_els);
        Box<2> bbox ( Box<2>::EMPTY_BOX );
        for(auto sei : surface_els)
        {
            auto sel = mesh[sei];
            for (auto i : Range(sel.GetNP()))
            {
                auto & gi = sel.GeomInfo()[i];
                Point<2> p = {gi.u, gi.v};
                bbox.Add(p);
            }
        }

        BoxTree<2> tree(bbox);

        for(auto pi : mesh.Points().Range())
        {
            auto n_surf_els = p2sel[pi].Size();

            bool has_relevant_sel = false;
            for(auto sei : p2sel[pi])
                if(mesh[sei].GetIndex()==facei)
                {
                    has_relevant_sel = true;
                    break;
                }
            if(!has_relevant_sel)
                continue;

            if(mesh[pi].Type() <= EDGEPOINT)
                edge_points.Append(pi);
            else
                surface_points.Append(pi);

            // the same point might have different uv coordinates (closed edges for instance)
            // duplicate these points for the delaunay tree
            bool inserted = false;
            for(auto sei : p2sel[pi])
            {
                auto sel = mesh[sei];
                if(sel.GetIndex()!=facei)
                    continue;

                PointGeomInfo gi = sel.GeomInfo()[sel.PNums().Pos(pi)];
                Point<2> p = {gi.u, gi.v};
                bool found = false;
                tree.GetFirstIntersecting( p, p, [&] (const auto pi_found) { return found = true; });
                if(found)
                    continue;

                auto pi_new = pi;
                if(inserted)
                {
                    pi_new = delaunay_points.Append(p);
                    pmap.Append(pi);
                    edge_points.Append(pi_new);
                }
                else
                {
                    delaunay_points[pi] = p;
                    pmap[pi] = pi;
                }
                tree.Insert(p, pi_new);
                inserted = true;
            }
        }

        if(surface_points.Size()==0)
            continue;

        DelaunayMesh dmesh( delaunay_points, bbox  );

        for(auto pi : edge_points)
            dmesh.AddPoint(pi);

        std::map<PointIndex, double> weights;
        for(auto pi : surface_points)
        {
            dmesh.CalcIntersecting(pi);
            dmesh.CalcWeights(pi, weights);
            auto & v = growthvectors[pi];
            auto n = 1./v.Length() * v;
            for(auto & [pi_other, weight] : weights)
              {
                // interpolate only the tangential part of the growth vector
                auto t = weight * growthvectors[pmap[pi_other]];
                t -= (t * n) * n;
                v += t;
              }
        }
      }
  }

  void GenerateBoundaryLayer(Mesh& mesh, const BoundaryLayerParameters& blp)
  {
    static Timer timer("Create Boundarylayers");
    RegionTimer regt(timer);

    bool interpolate_growth_vectors = false;
    if(mesh.GetGeometry())
        interpolate_growth_vectors = mesh.GetGeometry()->GetGeomType() == Mesh::GEOM_OCC;

    int max_edge_nr = -1;
    for(const auto& seg : mesh.LineSegments())
      if(seg.edgenr > max_edge_nr)
        max_edge_nr = seg.edgenr;

    int new_mat_nr = mesh.GetNDomains() +1;
    mesh.SetMaterial(new_mat_nr, blp.new_mat);

    auto domains = blp.domains;
    if(!blp.outside)
      domains.Invert();

    auto& meshtopo = mesh.GetTopology();
    meshtopo.SetBuildVertex2Element(true);
    mesh.UpdateTopology();

    bool have_single_segments = HaveSingleSegments(mesh);
    Array<Segment> segments;
    if(have_single_segments)
        segments = BuildSegments(mesh);
    else
        segments = mesh.LineSegments();

    int np = mesh.GetNP();
    int ne = mesh.GetNE();
    int nse = mesh.GetNSE();
    int nseg = segments.Size();

    Array<Array<PointIndex>, PointIndex> mapto(np);

    Array<Vec<3>, PointIndex> growthvectors(np);
    growthvectors = 0.;

    Array<double> surfacefacs(mesh.GetNFD()+1);
    surfacefacs = 0.;

    auto getSurfaceNormal = [&mesh] (const Element2d& el)
    {
      auto v0 = mesh[el[0]];
      return Cross(mesh[el[1]]-v0, mesh[el[2]]-v0).Normalize();
    };

    // surface index map
    Array<int> si_map(mesh.GetNFD()+1);
    si_map = -1;

    int fd_old = mesh.GetNFD();

    // create new FaceDescriptors
    for(auto i : Range(1, fd_old+1))
      {
        const auto& fd = mesh.GetFaceDescriptor(i);
        string name = fd.GetBCName();
        if(blp.surfid.Contains(i))
          {
            if(auto isIn = domains.Test(fd.DomainIn()); isIn != domains.Test(fd.DomainOut()))
              {
                int new_si = mesh.GetNFD()+1;
                surfacefacs[i] = isIn ? 1. : -1.;
                // -1 surf nr is so that curving does not do anything
                FaceDescriptor new_fd(-1, isIn ? new_mat_nr : fd.DomainIn(),
                                      isIn ? fd.DomainOut() : new_mat_nr, -1);
                new_fd.SetBCProperty(new_si);
                mesh.AddFaceDescriptor(new_fd);
                si_map[i] = new_si;
                mesh.SetBCName(new_si-1, "mapped_" + name);
              }
          }
      }

    const auto & p2sel = mesh.CreatePoint2SurfaceElementTable();
    Array<Vec<3>, PointIndex> edge_tangents(np);

    for(auto pi : mesh.Points().Range())
    {
        const auto & p = mesh[pi];
        if(p.Type() == INNERPOINT)
            continue;

        std::map<int, Vec<3>> normals;

        // calculate one normal vector per face (average with angles as weights for multiple surface elements within a face)
        for(auto sei : p2sel[pi])
        {
            const auto & sel = mesh[sei];
            auto facei = sel.GetIndex();
            if(!blp.surfid.Contains(facei))
                continue;

            auto n = surfacefacs[sel.GetIndex()] * getSurfaceNormal(sel);

            int itrig = sel.PNums().Pos(pi);
            itrig += sel.GetNP();
            auto v0 = (mesh[sel.PNumMod(itrig+1)] - mesh[pi]).Normalize();
            auto v1 = (mesh[sel.PNumMod(itrig-1)] - mesh[pi]).Normalize();
            if(normals.count(facei)==0)
                normals[facei] = {0.,0.,0.};
            normals[facei] += acos(v0*v1)*n;
        }

        for(auto & [facei, n] : normals)
            n *= 1.0/n.Length();

        // combine normal vectors for each face to keep uniform distances
        auto & np = growthvectors[pi];
        for(auto & [facei, n] : normals)
        {
            if(np.Length() == 0) { np = n; continue; }
            auto npn = np * n;
            auto npnp = np * np;
            auto nn = n * n;
            if(nn-npn*npn/npnp == 0) { np = n; continue; }
            edge_tangents[pi] = Cross(np, n).Normalize();
            np += (nn - npn)/(nn - npn*npn/npnp) * (n - npn/npnp * np);
        }
    }

    // Bit array to keep track of segments already processed
    BitArray segs_done(nseg+1);
    segs_done.Clear();

    // map for all segments with same points
    // points to pair of SegmentIndex, int
    // int is type of other segment, either:
    // 0 == adjacent surface grows layer
    // 1 == adjacent surface doesn't grow layer, but layer ends on it
    // 2 == adjacent surface is interior surface that ends on layer
    // 3 == adjacent surface is exterior surface that ends on layer (not allowed yet)
    Array<Array<pair<SegmentIndex, int>>, SegmentIndex> segmap(segments.Size());

    // moved segments
    Array<SegmentIndex> moved_segs;
    BitArray moved_edges(max_edge_nr+1);
    moved_edges = false;
    Array<Segment> new_segments;

    // boundaries to project endings to
    BitArray project_boundaries(fd_old+1);
    BitArray move_boundaries(fd_old+1);
    project_boundaries.Clear();
    move_boundaries.Clear();

    for(auto si : Range(segments))
      {
        if(segs_done[si]) continue;
        const auto& segi = segments[si];
        if(si_map[segi.si] == -1) continue;
        segs_done.SetBit(si);
        segmap[si].Append(make_pair(si, 0));
        moved_segs.Append(si);
        moved_edges.SetBit(segi.edgenr);
        for(auto sj : Range(segments))
          {
            if(segs_done.Test(sj)) continue;
            const auto& segj = segments[sj];
            if((segi[0] == segj[0] && segi[1] == segj[1]) ||
               (segi[0] == segj[1] && segi[1] == segj[0]))
              {
                segs_done.SetBit(sj);
                int type;
                if(si_map[segj.si] != -1)
                  type = 0;
                else if(const auto& fd = mesh.GetFaceDescriptor(segj.si); domains.Test(fd.DomainIn()) && domains.Test(fd.DomainOut()))
                  {
                    type = 2;
                    if(fd.DomainIn() == 0 || fd.DomainOut() == 0)
                      project_boundaries.SetBit(segj.si);
                  }
                else if(const auto& fd = mesh.GetFaceDescriptor(segj.si); !domains.Test(fd.DomainIn()) && !domains.Test(fd.DomainOut()))
                  {
                    type = 3;
                    if(fd.DomainIn() == 0 || fd.DomainOut() == 0)
                      project_boundaries.SetBit(segj.si);
                    move_boundaries.SetBit(segj.si);
                  }
                else
                  {
                    type = 1;
                    // in case 1 we project the growthvector onto the surface
                    project_boundaries.SetBit(segj.si);
                  }
                segmap[si].Append(make_pair(sj, type));
              }
          }
      }

    BitArray in_surface_direction(fd_old+1);
    in_surface_direction.Clear();

    // project growthvector on surface for inner angles
    if(blp.grow_edges)
      {
        for(const auto& sel : mesh.SurfaceElements())
          if(project_boundaries.Test(sel.GetIndex()))
            {
              auto n = getSurfaceNormal(sel);
              for(auto i : Range(sel.PNums()))
                {
                  auto pi = sel.PNums()[i];
                  if(growthvectors[pi].Length2() == 0.)
                    continue;
                  auto next = sel.PNums()[(i+1)%sel.GetNV()];
                  auto prev = sel.PNums()[i == 0 ? sel.GetNV()-1 : i-1];
                  auto v1 = (mesh[next] - mesh[pi]).Normalize();
                  auto v2 = (mesh[prev] - mesh[pi]).Normalize();
                  auto v3 = growthvectors[pi];
                  v3.Normalize();
                  auto tol = v1.Length() * 1e-12;
                  if((v1 * v3 > -tol) && (v2 * v3 > -tol))
                    in_surface_direction.SetBit(sel.GetIndex());
                  else
                    continue;

                  auto& g = growthvectors[pi];
                  auto ng = n * g;
                  auto gg = g * g;
                  auto nn = n * n;
                  // if(fabs(ng*ng-nn*gg) < 1e-12 || fabs(ng) < 1e-12) continue;
                  auto a = -ng*ng/(ng*ng-nn * gg);
                  auto b = ng*gg/(ng*ng-nn*gg);
                  g += a*g + b*n;
                }
            }
      }
    else
      {
        for(const auto& seg : segments)
          {
            int count = 0;
            for(const auto& seg2 : segments)
              if(((seg[0] == seg2[0] && seg[1] == seg2[1]) || (seg[0] == seg2[1] && seg[1] == seg2[0])) && blp.surfid.Contains(seg2.si))
                count++;
            if(count == 1)
              {
                growthvectors[seg[0]] = {0., 0., 0.};
                growthvectors[seg[1]] = {0., 0., 0.};
              }
          }
      }

    // interpolate tangential component of growth vector along edge
    if(interpolate_growth_vectors)
    for(auto edgenr : Range(max_edge_nr))
      {
        if(!moved_edges[edgenr+1]) continue;
        const auto& geo = *mesh.GetGeometry();
        if(edgenr >= geo.GetNEdges())
          continue;

        // build sorted list of edge
        Array<PointIndex> points;
        // find first vertex on edge
        double edge_len = 0.;
        for(const auto& seg : segments)
          {
            if(seg.edgenr-1 == edgenr && mesh[seg[0]].Type() == FIXEDPOINT)
              {
                points.Append(seg[0]);
                points.Append(seg[1]);
                edge_len += (mesh[seg[1]] - mesh[seg[0]]).Length();
                break;
              }
          }
        while(true)
          {
            bool point_found = false;
            for(auto si : meshtopo.GetVertexSegments(points.Last()))
              {
                const auto& seg = mesh[si];
                if(seg.edgenr-1 != edgenr)
                  continue;
                if(seg[0] == points.Last() && points[points.Size()-2] !=seg[1])
                  {
                    edge_len += (mesh[points.Last()] - mesh[seg[1]]).Length();
                    points.Append(seg[1]);
                    point_found = true;
                    break;
                  }
              }
            if(mesh[points.Last()].Type() == FIXEDPOINT)
              break;
            if(!point_found)
              throw Exception(string("Could not find connected list of line segements for edge ") + edgenr);
          }

        // tangential part of growth vectors
        auto gt1 = growthvectors[points[0]];
        auto gt2 = growthvectors[points.Last()];

        double len = 0.;
        for(size_t i = 1; i < points.Size()-1; i++)
          {
            auto pi = points[i];
            len += (mesh[pi] - mesh[points[i-1]]).Length();
            auto t = edge_tangents[pi];
            auto lam = len/edge_len;
            auto interpol = (1-lam) * (gt1 * t) * t + lam * (gt2 * t) * t;
            growthvectors[pi] += interpol;
          }
      }

    if(interpolate_growth_vectors)
        InterpolateSurfaceGrowthVectors(mesh, blp, fd_old, growthvectors, p2sel);

    // insert new points
    for (PointIndex pi = 1; pi <= np; pi++)
      if (growthvectors[pi].Length2() != 0)
        {
          Point<3> p = mesh[pi];
          for(auto i : Range(blp.heights))
            {
              p += blp.heights[i] * growthvectors[pi];
              mapto[pi].Append(mesh.AddPoint(p));
            }
        }

    // add 2d quads on required surfaces
    map<pair<PointIndex, PointIndex>, int> seg2edge;
    if(blp.grow_edges)
      {
        for(auto sei : moved_segs)
          {
            // copy here since we will add segments and this would
            // invalidate a reference!
            auto segi = segments[sei];
            for(auto [sej, type] : segmap[sei])
              {
                auto segj = segments[sej];
                if(type == 0)
                  {
                    Segment s;
                    s[0] = mapto[segj[0]].Last();
                    s[1] = mapto[segj[1]].Last();
                    s[2] = PointIndex::INVALID;
                    auto pair = s[0] < s[1] ? make_pair(s[0], s[1]) : make_pair(s[1], s[0]);
                    if(seg2edge.find(pair) == seg2edge.end())
                      seg2edge[pair] = ++max_edge_nr;
                    s.edgenr = seg2edge[pair];
                    s.si = si_map[segj.si];
                    new_segments.Append(s);
                  }
                // here we need to grow the quad elements
                else if(type == 1)
                  {
                    PointIndex pp1 = segj[1];
                    PointIndex pp2 = segj[0];
                    if(in_surface_direction.Test(segj.si))
                      {
                        Swap(pp1, pp2);
                        move_boundaries.SetBit(segj.si);
                      }
                    PointIndex p1 = pp1;
                    PointIndex p2 = pp2;
                    PointIndex p3, p4;
                    Segment s0;
                    s0[0] = p1;
                    s0[1] = p2;
                    s0[2] = PointIndex::INVALID;
                    s0.edgenr = segj.edgenr;
                    s0.si = segj.si;
                    new_segments.Append(s0);

                    for(auto i : Range(blp.heights))
                      {
                        Element2d sel(QUAD);
                        p3 = mapto[pp2][i];
                        p4 = mapto[pp1][i];
                        sel[0] = p1;
                        sel[1] = p2;
                        sel[2] = p3;
                        sel[3] = p4;
                        for(auto i : Range(4))
                        {
                            sel.GeomInfo()[i].u = 0.0;
                            sel.GeomInfo()[i].v = 0.0;
                        }
                        sel.SetIndex(segj.si);
                        mesh.AddSurfaceElement(sel);

                        // TODO: Too many, would be enough to only add outermost ones
                        Segment s1;
                        s1[0] = p2;
                        s1[1] = p3;
                        s1[2] = PointIndex::INVALID;
                        auto pair = make_pair(p2, p3);
                        if(seg2edge.find(pair) == seg2edge.end())
                          seg2edge[pair] = ++max_edge_nr;
                        s1.edgenr = seg2edge[pair];
                        s1.si = segj.si;
                        new_segments.Append(s1);
                        Segment s2;
                        s2[0] = p4;
                        s2[1] = p1;
                        s2[2] = PointIndex::INVALID;
                        pair = make_pair(p1, p4);
                        if(seg2edge.find(pair) == seg2edge.end())
                          seg2edge[pair] = ++max_edge_nr;
                        s2.edgenr = seg2edge[pair];
                        s2.si = segj.si;
                        new_segments.Append(s2);
                        p1 = p4;
                        p2 = p3;
                      }
                    Segment s3;
                    s3[0] = p3;
                    s3[1] = p4;
                    s3[2] = PointIndex::INVALID;
                    auto pair = p3 < p4 ? make_pair(p3, p4) : make_pair(p4, p3);
                    if(seg2edge.find(pair) == seg2edge.end())
                      seg2edge[pair] = ++max_edge_nr;
                    s3.edgenr = seg2edge[pair];
                    s3.si = segj.si;
                    new_segments.Append(s3);
                  }
              }
          }
      }

    BitArray fixed_points(np+1);
    fixed_points.Clear();
    BitArray moveboundarypoint(np+1);
    moveboundarypoint.Clear();
    for(SurfaceElementIndex si = 0; si < nse; si++)
      {
        // copy because surfaceels array will be resized!
        auto sel = mesh[si];
        if(si_map[sel.GetIndex()] != -1)
          {
            Array<PointIndex> points(sel.PNums());
            if(surfacefacs[sel.GetIndex()] > 0) Swap(points[0], points[2]);
            for(auto j : Range(blp.heights))
              {
                auto eltype = points.Size() == 3 ? PRISM : HEX;
                Element el(eltype);
                for(auto i : Range(points))
                    el[i] = points[i];
                for(auto i : Range(points))
                  points[i] = mapto[sel.PNums()[i]][j];
                if(surfacefacs[sel.GetIndex()] > 0) Swap(points[0], points[2]);
                for(auto i : Range(points))
                  el[sel.PNums().Size() + i] = points[i];
                el.SetIndex(new_mat_nr);
                mesh.AddVolumeElement(el);
              }
            Element2d newel = sel;
            for(auto& p : newel.PNums())
              p = mapto[p].Last();
            newel.SetIndex(si_map[sel.GetIndex()]);
            mesh.AddSurfaceElement(newel);
          }
        else
          {
            bool has_moved = false;
            for(auto p : sel.PNums())
              if(mapto[p].Size())
                has_moved = true;
            if(has_moved)
              for(auto p : sel.PNums())
                {
                  if(!mapto[p].Size())
                    {
                      fixed_points.SetBit(p);
                      if(move_boundaries.Test(sel.GetIndex()))
                        moveboundarypoint.SetBit(p);
                    }
                }
          }
        if(move_boundaries.Test(sel.GetIndex()))
          {
            for(auto& p : mesh[si].PNums())
              if(mapto[p].Size())
                p = mapto[p].Last();
          }
      }

    for(SegmentIndex sei = 0; sei < nseg; sei++)
      {
        auto& seg = segments[sei];
        if(move_boundaries.Test(seg.si))
          for(auto& p : seg.PNums())
            if(mapto[p].Size())
              p = mapto[p].Last();
      }

    for(ElementIndex ei = 0; ei < ne; ei++)
      {
        auto el = mesh[ei];
        ArrayMem<PointIndex,4> fixed;
        ArrayMem<PointIndex,4> moved;
        bool moved_bnd = false;
        for(const auto& p : el.PNums())
          {
            if(fixed_points.Test(p))
              fixed.Append(p);
            if(mapto[p].Size())
              moved.Append(p);
            if(moveboundarypoint.Test(p))
              moved_bnd = true;
          }

        bool do_move, do_insert;
        if(domains.Test(el.GetIndex()))
          {
            do_move = fixed.Size() && moved_bnd;
            do_insert = do_move;
          }
        else
          {
            do_move = !fixed.Size() || moved_bnd;
            do_insert = !do_move;
          }

        if(do_move)
          {
            for(auto& p : mesh[ei].PNums())
              if(mapto[p].Size())
                p = mapto[p].Last();
          }
        if(do_insert)
          {
            if(el.GetType() == TET)
              {
                if(moved.Size() == 3) // inner corner
                  {
                    PointIndex p1 = moved[0];
                    PointIndex p2 = moved[1];
                    PointIndex p3 = moved[2];
                    auto v1 = mesh[p1];
                    auto n = Cross(mesh[p2]-v1, mesh[p3]-v1);
                    auto d = mesh[mapto[p1][0]] - v1;
                    if(n*d > 0)
                      Swap(p2,p3);
                    PointIndex p4 = p1;
                    PointIndex p5 = p2;
                    PointIndex p6 = p3;
                    for(auto i : Range(blp.heights))
                      {
                        Element nel(PRISM);
                        nel[0] = p4; nel[1] = p5; nel[2] = p6;
                        p4 = mapto[p1][i]; p5 = mapto[p2][i]; p6 = mapto[p3][i];
                        nel[3] = p4; nel[4] = p5; nel[5] = p6;
                        nel.SetIndex(el.GetIndex());
                        mesh.AddVolumeElement(nel);
                      }
                  }
                if(moved.Size() == 2)
                  {
                    if(fixed.Size() == 1)
                      {
                        PointIndex p1 = moved[0];
                        PointIndex p2 = moved[1];
                        for(auto i : Range(blp.heights))
                          {
                            PointIndex p3 = mapto[moved[1]][i];
                            PointIndex p4 = mapto[moved[0]][i];
                            Element nel(PYRAMID);
                            nel[0] = p1;
                            nel[1] = p2;
                            nel[2] = p3;
                            nel[3] = p4;
                            nel[4] = el[0] + el[1] + el[2] + el[3] - fixed[0] - moved[0] - moved[1];
                            if(Cross(mesh[p2]-mesh[p1], mesh[p4]-mesh[p1]) * (mesh[nel[4]]-mesh[nel[1]]) > 0)
                              Swap(nel[1], nel[3]);
                            nel.SetIndex(el.GetIndex());
                            mesh.AddVolumeElement(nel);
                            p1 = p4;
                            p2 = p3;
                          }
                      }
                  }
                if(moved.Size() == 1 && fixed.Size() == 1)
                  {
                    PointIndex p1 = moved[0];
                    for(auto i : Range(blp.heights))
                      {
                        Element nel = el;
                        PointIndex p2 = mapto[moved[0]][i];
                        for(auto& p : nel.PNums())
                          {
                            if(p == moved[0])
                              p = p1;
                            else if(p == fixed[0])
                              p = p2;
                          }
                        p1 = p2;
                        mesh.AddVolumeElement(nel);
                      }
                  }
              }
            else if(el.GetType() == PYRAMID)
              {
                if(moved.Size() == 2)
                  {
                    if(fixed.Size() != 2)
                      throw Exception("This case is not implemented yet! Fixed size = " + ToString(fixed.Size()));
                    PointIndex p1 = moved[0];
                    PointIndex p2 = moved[1];
                    for(auto i : Range(blp.heights))
                      {
                        PointIndex p3 = mapto[moved[1]][i];
                        PointIndex p4 = mapto[moved[0]][i];
                        Element nel(PYRAMID);
                        nel[0] = p1;
                        nel[1] = p2;
                        nel[2] = p3;
                        nel[3] = p4;
                        nel[4] = el[0] + el[1] + el[2] + el[3] + el[4] - fixed[0] - fixed[1] - moved[0] - moved[1];
                        if(Cross(mesh[p2] - mesh[p1], mesh[p4]-mesh[p1]) * (mesh[nel[4]]-mesh[nel[1]]) > 0)
                          Swap(nel[1], nel[3]);
                        nel.SetIndex(el.GetIndex());
                        mesh.AddVolumeElement(nel);
                        p1 = p4;
                        p2 = p3;
                      }
                  }
                else if(moved.Size() == 1)
                  throw Exception("This case is not implemented yet!");
              }
            else
              throw Exception("Boundarylayer only implemented for tets and pyramids outside yet!");
          }
      }

    for(auto i : Range(1, fd_old+1))
      if(si_map[i] != -1)
        {
          if(mesh.GetFaceDescriptor(mesh.GetNFD()).DomainIn() == new_mat_nr)
            mesh.GetFaceDescriptor(i).SetDomainOut(new_mat_nr);
          else
            mesh.GetFaceDescriptor(i).SetDomainIn(new_mat_nr);
        }
    if(have_single_segments)
        MergeAndAddSegments(mesh, new_segments);
    else
    {
      for(auto & seg : new_segments)
        mesh.AddSegment(seg);
    }
    mesh.GetTopology().ClearEdges();
    mesh.UpdateTopology();
    mesh.SetGeometry(nullptr);
  }

  void AddDirection( Vec<3> & a, Vec<3> b )
  {
     if(a.Length2()==0.)
     {
        a = b;
        return;
     }

     if(b.Length2()==0.)
        return;

     auto ab = a * b;
     if(fabs(ab)>1-1e-8)
        return;

     Mat<2> m;
     m(0,0) = a[0];
     m(0,1) = a[1];
     m(1,0) = b[0];
     m(1,1) = b[1];
     Vec<2> lam;
     Vec<2> rhs;
     rhs[0] = a[0]-b[0];
     rhs[1] = a[1]-b[1];

     const auto Dot = [](Vec<3> a, Vec<3> b)
     { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };

     rhs[0] = Dot(a,a);
     rhs[1] = Dot(b,b);

     m.Solve(rhs, lam);
     a[0] = lam[0];
     a[1] = lam[1];
     a[2] = 0.0;
     return;
  }

  static void Generate2dMesh( Mesh & mesh, int domain )
  {
     Box<3> box{Box<3>::EMPTY_BOX};
     for(const auto & seg : mesh.LineSegments())
        if (seg.si == domain)
           for (auto pi : {seg[0], seg[1]})
              box.Add(mesh[pi]);

     MeshingParameters mp;
     Meshing2 meshing (*mesh.GetGeometry(), mp, box);

     Array<PointIndex, PointIndex> compress(mesh.GetNP());
     compress = PointIndex::INVALID;

     PointIndex cnt = PointIndex::BASE;
     for(const auto & seg : mesh.LineSegments())
        if (seg.si == domain)
           for (auto pi : {seg[0], seg[1]})
              if (compress[pi]==PointIndex{PointIndex::INVALID})
              {
                 meshing.AddPoint(mesh[pi], pi);
                 compress[pi] = cnt++;
              }

     PointGeomInfo gi;
     gi.trignum = domain;
     for(const auto & seg : mesh.LineSegments())
        if (seg.si == domain)
           meshing.AddBoundaryElement (compress[seg[0]], compress[seg[1]], gi, gi);

     auto oldnf = mesh.GetNSE();
     auto res = meshing.GenerateMesh (mesh, mp, mp.maxh, domain);
     for (SurfaceElementIndex sei : Range(oldnf, mesh.GetNSE()))
        mesh[sei].SetIndex (domain);

     int hsteps = mp.optsteps2d;

     const char * optstr = mp.optimize2d.c_str();
     MeshOptimize2d meshopt(mesh);
     meshopt.SetFaceIndex(domain);
     meshopt.SetMetricWeight (mp.elsizeweight);
     for (size_t j = 1; j <= strlen(optstr); j++)
     {
        switch (optstr[j-1])
        {
           case 's':
              {  // topological swap
                 meshopt.EdgeSwapping (0);
                 break;
              }
           case 'S':
              {  // metric swap
                 meshopt.EdgeSwapping (1);
                 break;
              }
           case 'm':
              {
                 meshopt.ImproveMesh(mp);
                 break;
              }
           case 'c':
              {
                 meshopt.CombineImprove();
                 break;
              }
           default:
              cerr << "Optimization code " << optstr[j-1] << " not defined" << endl;
        }
     }

     mesh.Compress();
     mesh.OrderElements();
     mesh.SetNextMajorTimeStamp();

  }

  int GenerateBoundaryLayer2 (Mesh & mesh, int domain, const Array<double> & thicknesses, bool should_make_new_domain, const Array<int> & boundaries)
  {
     SegmentIndex first_new_seg = mesh.LineSegments().Range().Next();

     int np = mesh.GetNP();
     int nseg = mesh.GetNSeg();
     int ne = mesh.GetNSE();
     mesh.UpdateTopology();

     double total_thickness = 0.0;
     for(auto thickness : thicknesses)
        total_thickness += thickness;

     Array<Array<PointIndex>, PointIndex> mapto(np);

     // Bit array to keep track of segments already processed
     BitArray segs_done(nseg);
     segs_done.Clear();

     // moved segments
     Array<SegmentIndex> moved_segs;

     Array<Vec<3>, PointIndex> growthvectors(np);
     growthvectors = 0.;

     auto & meshtopo = mesh.GetTopology();

     Array<SurfaceElementIndex, SegmentIndex> seg2surfel(mesh.GetNSeg());
     seg2surfel = -1;
     NgArray<SurfaceElementIndex> temp_els;
     for(auto si : Range(mesh.LineSegments()))
     {
        meshtopo.GetSegmentSurfaceElements ( si+1, temp_els );
        // NgArray<int> surfeledges;
        // meshtopo.GetSurfaceElementEdges(si+1, surfeledges);
        for(auto seli : temp_els)
           if(mesh[seli].GetIndex() == mesh[si].si)
              seg2surfel[si] = seli;
     }

     Array<SegmentIndex> segments;

    // surface index map
    Array<int> si_map(mesh.GetNFD()+1);
    si_map = -1;

    int fd_old = mesh.GetNFD();

    int max_edge_nr = -1;
    int max_domain = -1;

    for(const auto& seg : mesh.LineSegments())
    {
      if(seg.epgeominfo[0].edgenr > max_edge_nr)
        max_edge_nr = seg.epgeominfo[0].edgenr;
      if(seg.si > max_domain)
         max_domain = seg.si;
    }

    int new_domain = max_domain+1;

    BitArray active_boundaries(max_edge_nr+1);
    BitArray active_segments(nseg);
    active_boundaries.Clear();
    active_segments.Clear();

    if(boundaries.Size() == 0)
       active_boundaries.Set();
    else
       for(auto edgenr : boundaries)
          active_boundaries.SetBit(edgenr);

    for(auto segi : Range(mesh.LineSegments()))
    {
       const auto seg = mesh[segi];
       if(active_boundaries.Test(seg.epgeominfo[0].edgenr) && seg.si==domain)
          active_segments.SetBit(segi);
    }

    for(auto segi : Range(mesh.LineSegments()))
    {
        const auto& seg = mesh[segi];
        auto si = seg.si;

        if(si_map[si]!=-1)
           continue;

        if(!active_segments.Test(segi))
           continue;

        FaceDescriptor new_fd(0, 0, 0, -1);
        new_fd.SetBCProperty(new_domain);
        int new_fd_index = mesh.AddFaceDescriptor(new_fd);
        si_map[si] = new_domain;
        if(should_make_new_domain)
           mesh.SetBCName(new_domain-1, "mapped_" + mesh.GetBCName(si-1));
    }

    for(auto si : Range(mesh.LineSegments()))
      {
        if(segs_done[si]) continue;
        segs_done.SetBit(si);
        const auto& segi = mesh[si];
        if(si_map[segi.si] == -1) continue;
        if(!active_boundaries.Test(segi.epgeominfo[0].edgenr))
           continue;
        moved_segs.Append(si);
      }

     // calculate growth vectors (average normal vectors of adjacent segments at each point)
     for (auto si : moved_segs)
     {
       auto & seg = mesh[si];

       meshtopo.GetSegmentSurfaceElements ( si+1, temp_els );
       ArrayMem<int, 10> seg_domains;

       temp_els.SetSize(0);
       if(seg2surfel[si]!=-1)
          temp_els.Append(seg2surfel[si]);

       int n_temp_els = temp_els.Size();
       if(n_temp_els==0)
          continue;

       int dom0 = mesh[temp_els[0]].GetIndex();
       int dom1 = n_temp_els==2 ? mesh[temp_els[1]].GetIndex() : 0;

       bool in_dom0 = dom0 == domain;
       bool in_dom1 = dom1 == domain;

       if(!in_dom0 && !in_dom1)
          continue;

       int side = in_dom0 ? 0 : 1;

       auto & sel = mesh[ temp_els[side] ];

       int domain = sel.GetIndex();
       Vec<3> pcenter = 0.0;
       for(auto i : IntRange(sel.GetNP()))
       {
          for(auto d : IntRange(3))
             pcenter[d] += mesh[sel[i]][d];
       }
       pcenter = 1.0/sel.GetNP() * pcenter;

       auto n = mesh[seg[1]] - mesh[seg[0]];
       n = {-n[1], n[0], 0};
       n.Normalize();

       Vec<3> p0{mesh[seg[0]]};
       Vec<3> p1{mesh[seg[0]]};


       auto v = pcenter -0.5*(p0+p1);

       const auto Dot = [](Vec<3> a, Vec<3> b)
       { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
       if(Dot(n, v)<0)
          n = -1*n;

       AddDirection(growthvectors[seg[0]], n);
       AddDirection(growthvectors[seg[1]], n);
     }

     //////////////////////////////////////////////////////////////////////////
     // average growthvectors along straight lines to avoid overlaps in corners
     BitArray points_done(np+1);
     points_done.Clear();

     for(auto si : moved_segs)
     {
        auto current_seg = mesh[si];
        auto current_si = si;

        auto first = current_seg[0];
        auto current = -1;
        auto next =  current_seg[1];

        if(points_done.Test(first))
           continue;

        Array<PointIndex> chain;
        chain.Append(first);

        // first find closed loops of segments
        while(next != current && next != first)
        {
           current = next;
           points_done.SetBit(current);
           chain.Append(current);
           for(auto sj : meshtopo.GetVertexSegments( current ))
           {
              if(!active_segments.Test(sj))
                 continue;

              if(sj!=current_si)
              {
                 current_si = sj;
                 current_seg = mesh[sj];

                 next = current_seg[0] + current_seg[1] - current;
                 break;
              }
           }
        }

        auto ifirst = 0;
        auto n = chain.Size();

        // angle of adjacent segments at points a[i-1], a[i], a[i+1]
        auto getAngle = [&mesh, &growthvectors] (FlatArray<PointIndex> a, size_t i)
        {
           auto n = a.Size();
           auto v0 = growthvectors[a[(i+n-1)%n]];
           auto v1 = growthvectors[a[i]];
           auto v2 = growthvectors[a[(i+1)%n]];

           auto p0 = mesh[a[(i+n-1)%n]];
           auto p1 = mesh[a[i]];
           auto p2 = mesh[a[(i+1)%n]];

           v0 = p1-p0;
           v1 = p2-p1;

           auto angle = abs(atan2(v1[0], v1[1]) - atan2(v0[0], v0[1]));
           if(angle>M_PI)
              angle = 2*M_PI-angle;

           return angle;
        };

        // find first corner point
        while(getAngle(chain, ifirst) < 1e-5 )
           ifirst = (ifirst+1)%n;

        // Copy points of closed loop in correct order, starting with a corner
        Array<PointIndex> pis(n+1);
        pis.Range(0, n-ifirst) = chain.Range(ifirst, n);
        pis.Range(n-ifirst, n) = chain.Range(0, n-ifirst);
        pis[n] = pis[0];

        Array<double> lengths(n);

        for(auto i : Range(n))
           lengths[i] = (mesh[pis[(i+1)%n]] - mesh[pis[i]]).Length();

        auto averageGrowthVectors = [&] (size_t first, size_t last)
        {
           if(first+1 >= last)
              return;

           double total_len = 0.0;
           for(auto l : lengths.Range(first, last))
              total_len += l;

           double len = lengths[first];
           auto v0 = growthvectors[pis[first]];
           auto v1 = growthvectors[pis[last]];

           for(auto i : Range(first+1, last))
           {
              auto pi = pis[i];
              growthvectors[pi] = (len/total_len)*v1 + (1.0-len/total_len)*v0;
              len += lengths[i];
           }
        };

        auto icurrent = 0;

        while(icurrent<n)
        {
           auto ilast = icurrent+1;

           while(getAngle(pis, ilast) < 1e-5 && ilast < n)
              ilast++;

           // found straight line -> average growth vectors between end points
           if(icurrent!=ilast)
              averageGrowthVectors(icurrent, ilast);

           icurrent = ilast;
        }
     }

     //////////////////////////////////////////////////////////////////////
     // reduce growthvectors where necessary to avoid overlaps/slim regions
     const auto getSegmentBox = [&] (SegmentIndex segi)
     {
        PointIndex pi0=mesh[segi][0], pi1=mesh[segi][1];
        Box<3> box( mesh[pi0], mesh[pi1] );
        box.Add( mesh[pi0]+growthvectors[pi0] );
        box.Add( mesh[pi1]+growthvectors[pi1] );
        return box;
     };

     Array<double, PointIndex> growth(np);
     growth = 1.0;

     const auto Dot = [](auto a, auto b)
     { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };

     const auto restrictGrowthVectors = [&] (SegmentIndex segi0, SegmentIndex segi1)
     {
        if(!active_segments.Test(segi0))
           return;

        const auto & seg0 = mesh[segi0];
        const auto & seg1 = mesh[segi1];

        if(seg0.si != seg1.si)
           return;

        if(segi0 == segi1)
           return;

        if(seg0[0]==seg1[0] || seg0[0]==seg1[1] || seg0[1]==seg1[0] || seg0[1] == seg1[1])
           return;

        auto n = mesh[seg0[0]] - mesh[seg0[1]];
        n = {-n[1], n[0], 0};
        n.Normalize();
        if(Dot(n, growthvectors[seg0[0]])<0) n = -n;
        if(Dot(n, growthvectors[seg0[1]])<0) n = -n;

        auto n1 = mesh[seg1[0]] - mesh[seg1[1]];
        n1 = {-n1[1], n1[0], 0};
        n1.Normalize();
        if(Dot(n1, growthvectors[seg1[0]])<0) n1 = -n;
        if(Dot(n1, growthvectors[seg1[1]])<0) n1 = -n;

        auto p10 = mesh[seg1[0]];
        auto p11 = mesh[seg1[1]];

        for ( auto pi : {seg0[0], seg0[1]} )
        {
           if(growthvectors[pi] == 0.0)
              continue;

           PointIndex pi1 = seg0[0] + seg0[1] - pi;
           auto p1 = mesh[pi1];
           auto p = mesh[pi];

           Point<3> points[] = { p10, p11, p10+total_thickness*growthvectors[seg1[0]], p11+total_thickness*growthvectors[seg1[1]], p1+total_thickness*growthvectors[pi1] };

           Vec<3> gn{ growthvectors[pi][1], -growthvectors[pi][0], 0.0 };
           if(Dot(gn, p1-p) < 0)
              gn = -gn;

           double d0 = Dot(gn, p);
           double d1 = Dot(gn, p1);
           if(d0>d1)
              Swap(d0,d1);

           bool all_left=true, all_right=true;

           for (auto i: Range(4))
           {
              auto p_other = points[i];
              auto dot = Dot(gn,p_other);
              if(dot>d0) all_left = false;
              if(dot<d1) all_right = false;
           }

           if(all_left || all_right)
              return;

           //for ( auto pi : {seg0[0], seg0[1]} )
           {
              double safety = 1.3;
              double t = safety*total_thickness;
              if(growthvectors[pi] == 0.0)
                 continue;

              Point<3> points[] = { p10, p10+t*growthvectors[seg1[0]], p11, p11+t*growthvectors[seg1[1]] };
              auto p0 = mesh[pi];
              auto p1 = p0 + t*growthvectors[pi];
              auto P2 = [](Point<3> p) { return Point<2>{p[0], p[1]}; };
              ArrayMem<pair<double, double>, 4> intersections;

              double alpha, beta;

              if(X_INTERSECTION == intersect( P2(p0), P2(p1), P2(points[0]), P2(points[2]), alpha, beta ))
                 intersections.Append({alpha, 0.0});

              if(X_INTERSECTION == intersect( P2(p0), P2(p1), P2(points[1]), P2(points[3]), alpha, beta ))
                 intersections.Append({alpha, 1.0});

              if(X_INTERSECTION == intersect( P2(p0), P2(p1), P2(points[0]), P2(points[1]), alpha, beta ))
                 intersections.Append({alpha, beta});

              if(X_INTERSECTION == intersect( P2(p0), P2(p1), P2(points[2]), P2(points[3]), alpha, beta ))
                 intersections.Append({alpha, beta});

              QuickSort(intersections);
              for(auto [alpha,beta] : intersections)
              {
                 if(!active_segments.Test(segi1))
                    growth[pi] = min(growth[pi], alpha);
                 else
                 {
                    double mean = 0.5*(alpha+beta);
                    growth[pi] = min(growth[pi], mean);
                    growth[seg1[0]] = min(growth[seg1[0]], mean);
                    growth[seg1[1]] = min(growth[seg1[1]], mean);
                 }
              }
           }
        }
     };

     Box<3> box(Box<3>::EMPTY_BOX);
     for (auto segi : Range(mesh.LineSegments()))
     {
        auto segbox = getSegmentBox( segi );
        box.Add(segbox.PMin());
        box.Add(segbox.PMax());
     }
     BoxTree<3> segtree(box);

     for (auto segi : Range(mesh.LineSegments()))
     {
        auto p2 = [](Point<3> p) { return Point<2>{p[0], p[1]}; };

        auto seg = mesh[segi];
        double alpha,beta;
        intersect( p2(mesh[seg[0]]), p2(mesh[seg[0]]+total_thickness*growthvectors[seg[0]]), p2(mesh[seg[1]]), p2(mesh[seg[1]]+total_thickness*growthvectors[seg[1]]), alpha, beta );

        if(beta>0 && alpha>0 && alpha<1.1)
           growth[seg[0]] = min(growth[seg[0]], 0.8*alpha);
        if(alpha>0 && beta>0 && beta<1.1)
           growth[seg[1]] = min(growth[seg[1]], 0.8*beta);

        for (auto segj : Range(mesh.LineSegments()))
           if(segi!=segj)
              restrictGrowthVectors(segi, segj);
     }

     for( auto pi : Range(growthvectors))
        growthvectors[pi] *= growth[pi];


     // insert new points
     for(PointIndex pi : Range(mesh.Points()))
        if(growthvectors[pi].Length2()!=0)
        {

           auto & pnew = mapto[pi];
           auto dist = 0.0;
           for(auto t : thicknesses)
           {
              dist+=t;
              pnew.Append( mesh.AddPoint( mesh[pi] + dist*growthvectors[pi] ) );
              mesh[pnew.Last()].SetType(FIXEDPOINT);
           }
        }

     map<pair<PointIndex, PointIndex>, int> seg2edge;

     // insert new elements ( and move old ones )
     for(auto si : moved_segs)
     {
        auto seg = mesh[si];

        bool swap = false;
        auto & pm0 = mapto[seg[0]];
        auto & pm1 = mapto[seg[1]];

        auto newindex = si_map[seg.si];

        Segment s = seg;
        s.geominfo[0] = {};
        s.geominfo[1] = {};
        s[0] = pm0.Last();
        s[1] = pm1.Last();
        s[2] = PointIndex::INVALID;
        auto pair = s[0] < s[1] ? make_pair(s[0], s[1]) : make_pair(s[1], s[0]);
        if(seg2edge.find(pair) == seg2edge.end())
           seg2edge[pair] = ++max_edge_nr;
        s.edgenr = seg2edge[pair];
        s.si = seg.si;
        mesh.AddSegment(s);

        Swap(s[0], s[1]);
        s.si =  newindex;
        mesh.AddSegment(s);

        for ( auto i : Range(thicknesses))
        {
           PointIndex pi0, pi1, pi2, pi3;

           if(i==0)
           {
              pi0 = seg[0];
              pi1 = seg[1];
           }
           else
           {
              pi0 = pm0[i-1];
              pi1 = pm1[i-1];
           }

           pi2 = pm1[i];
           pi3 = pm0[i];

           if(i==0)
           {
              auto p0 = mesh[pi0];
              auto p1 = mesh[pi1];
              auto q0 = mesh[pi2];
              auto q1 = mesh[pi3];

              Vec<2> n = {-p1[1]+p0[1], p1[0]-p0[0]};
              Vec<2> v = { q0[0]-p0[0], q0[1]-p0[1]};
              if(n[0]*v[0]+n[1]*v[1]<0)
                 swap = true;
           }

           Element2d newel;
           newel.SetType(QUAD);
           newel[0] = pi0;
           newel[1] = pi1;
           newel[2] = pi2;
           newel[3] = pi3;
           newel.SetIndex(si_map[seg.si]);
           newel.GeomInfo() = PointGeomInfo{};

//            if(swap)
//            {
//               Swap(newel[0], newel[1]);
//               Swap(newel[2], newel[3]);
//            }

           for(auto i : Range(4))
           {
               newel.GeomInfo()[i].u = 0.0;
               newel.GeomInfo()[i].v = 0.0;
           }
           mesh.AddSurfaceElement(newel);

        }
        // segment now adjacent to new 2d-domain!
        mesh[si].si = si_map[seg.si];
     }

     for(auto pi : Range(mapto))
     {
        if(mapto[pi].Size() == 0)
           continue;
        auto pnew = mapto[pi].Last();
        for(auto old_sei : meshtopo.GetVertexSurfaceElements( pi ))
        {
           if(mesh[old_sei].GetIndex() == domain)
           {
              auto & old_el = mesh[old_sei];
              for(auto i : IntRange(old_el.GetNP()))
                 if(old_el[i]==pi)
                    old_el[i] = pnew;
           }
        }
     }

     for(auto & sel : mesh.SurfaceElements())
        if(sel.GetIndex() == domain)
           sel.Delete();

     mesh.Compress();
     mesh.CalcSurfacesOfNode();

     Generate2dMesh(mesh, domain);

     // even without new domain, we need temporarily a new domain to mesh the remaining area, without confusing the meshes with quads -> add segments temporarily and reset domain number and segments afterwards
     if(!should_make_new_domain)
     {
        // map new domain back to old one
        for(auto & sel : mesh.SurfaceElements())
           if(sel.GetIndex()==new_domain)
              sel.SetIndex(domain);

        // remove (temporary) inner segments
        for(auto segi : Range(first_new_seg, mesh.LineSegments().Range().Next()))
        {
           mesh[segi][0].Invalidate();
           mesh[segi][1].Invalidate();
        }

        for(auto segi : moved_segs)
           mesh[segi].si = domain;

        mesh.Compress();
        mesh.CalcSurfacesOfNode();
     }

     return new_domain;
   }

}
