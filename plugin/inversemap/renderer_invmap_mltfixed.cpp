/*
    Lightmetrica - A modern, research-oriented renderer

    Copyright (c) 2015 Hisanari Otsu

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.
*/

#include "inversemaputils.h"
#include <boost/format.hpp>

LM_NAMESPACE_BEGIN

struct TwoTailedGeometricDist
{
    Float base;
    Float invLogBase;
    Float baseNormalization;

    int center, start, end;
    Float offset;
    Float normalization;

public:

    TwoTailedGeometricDist(Float base)
        : base(base)
    {
        baseNormalization = 1_f / (base + 1_f);
        invLogBase = 1_f / std::log(base);
    }

public:

    auto Configure(int center, int start, int end) -> void
    {
        this->center = center;
        this->start = start - center;
        this->end = end - center;
        offset = R(this->start - 1);
        normalization = R(this->end) - offset;
    }

    auto EvaluatePDF(int i) const -> Float
    {
        i -= center;
        if (i < start || i > end) { return 0_f; }
        return r(i) / normalization;
    }

    auto EvaluateCDF(int i) const -> Float
    {
        i -= center;
        if (i < start) { return 0_f; }
        else if (i > end) { i = end; }
        return (R(i) - offset) / normalization;
    }

    auto Sample(Float u) const -> int
    {
        // For rare case u=1 generates divide by zero exception
        u = Math::Clamp(u, 0_f, 1_f - Math::Eps());
        return Math::Max(start, Rinv(u * normalization + offset)) + center;
    }

private:

    auto r(int i) const -> Float
    {
        //RF_DISABLE_FP_EXCEPTION();
        const Float t = (base - 1_f) * baseNormalization * std::pow(base, -std::abs((Float)(i)));
        //RF_ENABLE_FP_EXCEPTION();
        return t;
    }

    auto R(int i) const -> Float
    {
        //RF_DISABLE_FP_EXCEPTION();
        const Float t = i <= 0 ? std::pow(base, (Float)(i + 1)) * baseNormalization : 1_f - std::pow(base, -(Float)(i)) * baseNormalization;
        //RF_ENABLE_FP_EXCEPTION();
        return t;
    }

    auto Rinv(Float x) const -> int
    {
        Float result;
        if (x < base * baseNormalization)
        {
            result = std::log((1_f + base) * x) * invLogBase - 1_f;
        }
        else
        {
            result = -std::log((1_f + base) * (1_f - x)) * invLogBase;
        }
        return static_cast<int>(std::ceil(result));
    }

};

///! Metropolis light transport (fixed path length)
class Renderer_Invmap_MLTFixed final : public Renderer
{
public:

    LM_IMPL_CLASS(Renderer_Invmap_MLTFixed, Renderer);

public:

    int numVertices_;
    long long numMutations_;
    long long numSeedSamples_;

public:

    LM_IMPL_F(Initialize) = [this](const PropertyNode* prop) -> bool
    {
        if (!prop->ChildAs<int>("num_vertices", numVertices_)) return false;
        if (!prop->ChildAs<long long>("num_mutations", numMutations_)) return false;
        if (!prop->ChildAs<long long>("num_seed_samples", numSeedSamples_)) return false;
        return true;
    };

    LM_IMPL_F(Render) = [this](const Scene* scene, Random* initRng, Film* film) -> void
    {
        #pragma region Compute normalization factor
        const auto b = [&]() -> Float
        {
            LM_LOG_INFO("Computing normalization factor");
            LM_LOG_INDENTER();
            
            struct Context
            {
                Random rng;
                Float b = 0_f;
            };
            std::vector<Context> contexts(Parallel::GetNumThreads());
            for (auto& ctx : contexts) { ctx.rng.SetSeed(initRng->NextUInt()); }
            
            Parallel::For(numSeedSamples_, [&](long long index, int threadid, bool init)
            {
                auto& ctx = contexts[threadid];
                
                // Generate primary sample
                std::vector<Float> ps;
                for (int i = 0; i < InversemapUtils::NumSamples(numVertices_); i++)
                {
                    ps.push_back(ctx.rng.Next());
                }

                // Map to path
                const auto p = InversemapUtils::MapPS2Path(scene, ps);
                if (!p || p->vertices.size() != numVertices_)
                {
                    return;
                }

                // Accumulate contribution
                ctx.b += (p->EvaluateF(0) / p->EvaluatePathPDF(scene, 0)).Luminance();
            });

            Float b = 0_f;
            for (auto& ctx : contexts) { b += ctx.b; }
            b /= numSeedSamples_;

            LM_LOG_INFO(boost::str(boost::format("Normalization factor: %.10f") % b));
            return b;
        }();
        #pragma endregion

        // --------------------------------------------------------------------------------

        #pragma region Rendering
        {
            LM_LOG_INFO("Rendering");
            LM_LOG_INDENTER();

            // --------------------------------------------------------------------------------

            // Thread-specific context
            struct Context
            {
                Random rng;
                Film::UniquePtr film{ nullptr, nullptr };
                Path currP;
            };
            std::vector<Context> contexts(Parallel::GetNumThreads());
            for (auto& ctx : contexts)
            {
                ctx.rng.SetSeed(initRng->NextUInt());
                ctx.film = ComponentFactory::Clone<Film>(film);

                // Initial state
                while (true)
                {
                    // Generate initial sample with positive contribution with path tracing
                    // Ignore start-up bias here
                    std::vector<Float> ps;
                    for (int i = 0; i < InversemapUtils::NumSamples(numVertices_); i++)
                    {
                        ps.push_back(initRng->Next());
                    }

                    const auto path = InversemapUtils::MapPS2Path(scene, ps);
                    if (!path || path->vertices.size() != numVertices_ || path->EvaluateF(0).Black())
                    {
                        continue;
                    }

                    ctx.currP = path;
                    break;
                }
            }

            // --------------------------------------------------------------------------------

            Parallel::For(numMutations_, [&](long long index, int threadid, bool init) -> void
            {
                auto& ctx = contexts[threadid];

                // --------------------------------------------------------------------------------

                #pragma region Mutate the current path

                // Bidirectional mutation first narrows the mutation space by limiting the deleted range
                // in the current path, so it requires some additional information other than proposed path itself.
                struct Prop
                {
                    Path p;
                    int kd;
                    int dL;
                };

                const auto prop = [&]() -> boost::optional<Prop>
                {
                    // Implements bidirectional mutation within same path length
                    // Some simplification
                    //   - Mutation within the same path length
                
                    const int n = (int)(ctx.curr.vertices.size());

                    // Choose # of path vertices to be deleted
                    TwoTailedGeometricDist removedPathVertexNumDist(2);
                    removedPathVertexNumDist.Configure(1, 1, n);
                    const int kd = removedPathVertexNumDist.Sample(ctx.rng.Next());

                    // Choose range of deleted vertices [dL,dM]
                    const int dL = Math::Clamp((int)(ctx.rng.Next() * (n - kd + 1)), 0, n - kd);
                    const int dM = dL + kd - 1;

                    // Choose # of verticed added from each endpoint
                    const int aL = Math::Clamp((int)(ctx.rng.Next() * (kd + 1)), 0, kd);
                    const int aM = kd - aL;

                    // Sample subpaths
                    Path subpathL;
                    for (int s = 0; s < dL; s++)
                    {
                        subpathL.vertices.push_back(ctx.curr.vertices[s]);
                    }
                    if (subpathL.SampleVerticesFromEndpoint(scene, ctx.rng, PrimitiveType::L, aL) != aL)
                    {
                        return boost::none;
                    }

                    Path subpathE;
                    for (int t = n - 1; t > dM; t--)
                    {
                        subpathE.vertices.push_back(ctx.curr.vertices[t]);
                    }
                    if (subpathE.SampleVerticesFromEndpoint(scene, ctx.rng, PrimitiveType::E, aM) != aM)
                    {
                        return boost::none;
                    }

                    // Create proposed path
                    Prop prop;
                    if (!prop.p.Connect(scene, (int)(subpathL.vertices.size()), (int)(subpathE.vertices.size()), subpathL, subpathE))
                    {
                        return boost::none;
                    }

                    prop.kd = kd;
                    prop.dL = dL;
                    return prop;
                }();

                const auto Q = [](const Path& x, const Path& y, int kd, int dL) -> SPD
                {
                    SPD sum = 0;
                    for (int i = 0; i <= kd; i++)
                    {
                        const auto C = y.EvaluateF(dL + i) / y.EvaluatePathPDF(scene, dL + i);
                        if (C.Black())
                        {
                            continue;
                        }
                        sum += 1.0 / C;
                    }
                    if (sum.Black())
                    {
                        return SPD();
                    }
                    return sum;
                };

                #pragma endregion

                // --------------------------------------------------------------------------------

                #pragma region MH update

                #if 0
                if (prop)
                {
                    // Proposal distribution is symmetric
                    const double Fx = ctx.curr.EvaluateF();
                    const double Fy = prop->p.EvaluateF();

                    double A = 0;
                    if (Fx <= 0 || Fy <= 0 || glm::isnan(Fx) || glm::isnan(Fy))
                    {
                        A = 0;
                    }
                    else
                    {
                        A = std::min(1.0, Fy / Fx);
                    }

                    if (ctx.rng.Next() < A)
                    {
                        ctx.curr = *prop;
                    }
                }
                #endif

                if (prop)
                {
                    const double Qxy = Q(ctx.currP, prop->p, prop->kd, prop->dL);
                    const double Qyx = Q(prop->p, ctx.currP, prop->kd, prop->dL);
                    double A = 0;
                    if (Qxy <= 0 || Qyx <= 0 || std::isnan(Qxy) || std::isnan(Qyx))
                    {
                        A = 0;
                    }
                    else
                    {
                        A = std::min(1.0, Qyx / Qxy);
                    }
                    if (ctx.rng.Next() < A)
                    {
                        ctx.currP = prop->p;
                    }
                }

                #pragma endregion

                // --------------------------------------------------------------------------------

                #pragma region Accumulate contribution
                {
                    const auto currF = ctx.currP.EvaluateF(0);
                    if (!currF.Black())
                    {
                        const auto I = (currF / ctx.currP.EvaluatePathPDF(scene, 0)).Luminance();
                        ctx.film->Splat(ctx.currP.RasterPosition(), b / I);
                    }
                }
                #pragma endregion
            });

            // --------------------------------------------------------------------------------

            // Gather & Rescale
            film->Clear();
            for (auto& ctx : contexts)
            {
                film->Accumulate(ctx.film.get());
            }
            film->Rescale((Float)(film->Width() * film->Height()) / numMutations_);
        }
        #pragma endregion

    };

};

LM_COMPONENT_REGISTER_IMPL(Renderer_Invmap_MLTFixed, "renderer::invmap_mltfixed");

LM_NAMESPACE_END
