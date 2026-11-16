// Cold-overlay code called from fibers on the codegen core.
extern "C" double ColdStep(double accumulator, int step)
{
    return accumulator * 0.5 + static_cast<double>(step) * 0.25;
}
