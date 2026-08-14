#include <exception>
#include <iostream>

void RunRtPrimitiveTests();
void RunLifecycleTests();
void RunThreadSafetyTests();

int main()
{
  try
  {
    RunRtPrimitiveTests();
    RunLifecycleTests();
    RunThreadSafetyTests();
    std::cout << "Concurrency and lifecycle tests passed.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Concurrency test failure: " << ex.what() << '\n';
    return 1;
  }
}
