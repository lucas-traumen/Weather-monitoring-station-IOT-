################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../ascon/aead.c \
../ascon/permutations.c \
../ascon/printstate.c 

OBJS += \
./ascon/aead.o \
./ascon/permutations.o \
./ascon/printstate.o 

C_DEPS += \
./ascon/aead.d \
./ascon/permutations.d \
./ascon/printstate.d 


# Each subdirectory must supply rules for building sources it contributes
ascon/%.o ascon/%.su ascon/%.cyclo: ../ascon/%.c ascon/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m3 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F103xB -c -I../Core/Inc -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor" -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor/bmp388" -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/sensor/sht30" -I"D:/stm32workspacecubeide/bulepill/Driver/CMSIS/DSP/Include" -I../Drivers/STM32F1xx_HAL_Driver/Inc -I../Drivers/STM32F1xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F1xx/Include -I../Drivers/CMSIS/Include -I"/home/lucas/Desktop/Github/Project_1/Weather-monitoring-station-IOT-/firmware/stm32/node_stm32_ver1/ascon" -Os -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -fshort-enums -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfloat-abi=soft -mthumb -o "$@"

clean: clean-ascon

clean-ascon:
	-$(RM) ./ascon/aead.cyclo ./ascon/aead.d ./ascon/aead.o ./ascon/aead.su ./ascon/permutations.cyclo ./ascon/permutations.d ./ascon/permutations.o ./ascon/permutations.su ./ascon/printstate.cyclo ./ascon/printstate.d ./ascon/printstate.o ./ascon/printstate.su

.PHONY: clean-ascon

